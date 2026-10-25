// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/gles/context_gles.h"
#include <memory>

#include "impeller/base/config.h"
#include "impeller/base/validation.h"
#include "impeller/base/version.h"
#include "impeller/core/runtime_types.h"
#include "impeller/renderer/backend/gles/command_buffer_gles.h"
#include "impeller/renderer/backend/gles/gpu_tracer_gles.h"
#include "impeller/renderer/backend/gles/handle_gles.h"
#include "impeller/renderer/backend/gles/render_pass_gles.h"
#include "impeller/renderer/backend/gles/texture_gles.h"
#include "impeller/renderer/command_queue.h"

namespace impeller {

std::shared_ptr<ContextGLES> ContextGLES::Create(
    const Flags& flags,
    std::unique_ptr<ProcTableGLES> gl,
    const std::vector<std::shared_ptr<fml::Mapping>>& shader_libraries,
    bool enable_gpu_tracing,
    std::shared_ptr<fml::BasicTaskRunner> io_task_runner,
    std::optional<AvioAntialiasingConfig> antialiasing_config) {
  return std::shared_ptr<ContextGLES>(new ContextGLES(
      flags, std::move(gl), shader_libraries, enable_gpu_tracing,
      std::move(io_task_runner), antialiasing_config));
}

ContextGLES::ContextGLES(
    const Flags& flags,
    std::unique_ptr<ProcTableGLES> gl,
    const std::vector<std::shared_ptr<fml::Mapping>>& shader_libraries_mappings,
    bool enable_gpu_tracing,
    std::shared_ptr<fml::BasicTaskRunner> io_task_runner,
    std::optional<AvioAntialiasingConfig> antialiasing_config)
    : Context(flags),
      antialiasing_config_(
          antialiasing_config.value_or(AvioAntialiasingConfig{})) {
  if ((antialiasing_config_.continuous_requested_classes &
       ~AvioContinuousSupportedClasses(AvioCoverageBackend::kGLES)) != 0) {
    VALIDATION_LOG << "Requested continuous coverage is unsupported on GLES.";
    return;
  }
  reactor_ = std::make_shared<ReactorGLES>(std::move(gl));
  if (!reactor_->IsValid()) {
    VALIDATION_LOG << "Could not create valid reactor.";
    return;
  }

  if (antialiasing_config_.UsesCoverage() &&
      !reactor_->GetProcTable()
           .GetCapabilities()
           ->SupportsAvioCoverageResources()) {
    VALIDATION_LOG
        << "GLES native four-sample coverage resources are unsupported.";
    return;
  }

  // Create the shader library.
  {
    auto library = std::shared_ptr<ShaderLibraryGLES>(
        new ShaderLibraryGLES(shader_libraries_mappings));
    if (!library->IsValid()) {
      VALIDATION_LOG << "Could not create valid shader library.";
      return;
    }
    shader_library_ = std::move(library);
  }

  // Create the pipeline library.
  {
    pipeline_library_ = std::shared_ptr<PipelineLibraryGLES>(
        new PipelineLibraryGLES(reactor_, std::move(io_task_runner)));
  }

  // Create allocators.
  {
    resource_allocator_ =
        std::shared_ptr<AllocatorGLES>(new AllocatorGLES(reactor_));
    if (!resource_allocator_->IsValid()) {
      VALIDATION_LOG << "Could not create a resource allocator.";
      return;
    }
  }

  device_capabilities_ = reactor_->GetProcTable().GetCapabilities();

  // Create the sampler library.
  {
    sampler_library_ =
        std::shared_ptr<SamplerLibraryGLES>(new SamplerLibraryGLES(
            device_capabilities_->SupportsDecalSamplerAddressMode()));
  }
  gpu_tracer_ = std::make_shared<GPUTracerGLES>(GetReactor()->GetProcTable(),
                                                enable_gpu_tracing);
  command_queue_ = std::make_shared<CommandQueue>();
  is_valid_ = true;
}

ContextGLES::~ContextGLES() = default;

Context::BackendType ContextGLES::GetBackendType() const {
  return Context::BackendType::kOpenGLES;
}

const AvioAntialiasingConfig& ContextGLES::GetAvioAntialiasingConfig() const {
  return antialiasing_config_;
}

AvioRenderResourceReport ContextGLES::GetAvioRenderResourceReport(
    bool start_new_interval) const {
  // GL has no portable physical texture/program allocation query.
  // ContentContext supplies the descriptor-only warm-region inventory with
  // field availability.
  AvioRenderResourceReport report;
  report.available = IsValid();
  if (report.available) {
    report.Merge(GetAvioCoverageRegionResourceReport(start_new_interval));
    report.Merge(GetAvioCoverageUsageReport(start_new_interval));
    std::lock_guard lock(external_targets_mutex_);
    AvioRenderResourceEntry bindings;
    bindings.kind_id =
        static_cast<uint32_t>(AvioRenderResourceKind::kExternalTargetBindings);
    bindings.fields_supported =
        kAvioResourceFieldCounts | kAvioResourceFieldLeases;
    bindings.unsupported_reason_id =
        kAvioResourceReasonPhysicalAllocationUnavailable;
    bindings.usage.entries = external_targets_.size();
    bindings.usage.created_entries = external_targets_created_;
    bindings.usage.orphans_released_entries = external_targets_retired_;
    for (const auto& [id, target] : external_targets_) {
      bindings.usage.leased_entries += target.use_count() > 1 ? 1 : 0;
    }
    report.AddEntry(bindings);
    if (start_new_interval) {
      external_targets_created_ = 0;
      external_targets_retired_ = 0;
    }
  }
  return report;
}

const std::shared_ptr<ReactorGLES>& ContextGLES::GetReactor() const {
  return reactor_;
}

std::optional<ReactorGLES::WorkerID> ContextGLES::AddReactorWorker(
    const std::shared_ptr<ReactorGLES::Worker>& worker) {
  if (!IsValid()) {
    return std::nullopt;
  }
  return reactor_->AddWorker(worker);
}

bool ContextGLES::RemoveReactorWorker(ReactorGLES::WorkerID id) {
  if (!IsValid()) {
    return false;
  }
  return reactor_->RemoveWorker(id);
}

bool ContextGLES::IsValid() const {
  return is_valid_ && reactor_->IsValid() &&
         !external_completion_unobservable_.load();
}

void ContextGLES::Shutdown() {
  reactor_->Shutdown();
}

void ContextGLES::MarkExternalCompletionUnobservable() {
  external_completion_unobservable_.store(true);
}

bool ContextGLES::IsSafeToDestroyNativeResources() const {
  // The host must also observe every successfully exported completion fence.
  // Failure is sticky: thread joins and reactor shutdown cannot recover it.
  std::lock_guard lock(external_targets_mutex_);
  return !external_completion_unobservable_.load() && external_targets_.empty();
}

// |Context|
std::string ContextGLES::DescribeGpuModel() const {
  return reactor_->GetProcTable().GetDescription()->GetString();
}

// |Context|
std::shared_ptr<Allocator> ContextGLES::GetResourceAllocator() const {
  return resource_allocator_;
}

std::shared_ptr<const GpuSubmissionTracker> ContextGLES::GetSubmissionTracker()
    const {
  return submission_tracker_;
}

const std::shared_ptr<GpuSubmissionTracker>&
ContextGLES::GetMutableSubmissionTracker() const {
  return submission_tracker_;
}

// |Context|
std::shared_ptr<ShaderLibrary> ContextGLES::GetShaderLibrary() const {
  return shader_library_;
}

// |Context|
std::shared_ptr<SamplerLibrary> ContextGLES::GetSamplerLibrary() const {
  return sampler_library_;
}

// |Context|
std::shared_ptr<PipelineLibrary> ContextGLES::GetPipelineLibrary() const {
  return pipeline_library_;
}

// |Context|
std::shared_ptr<CommandBuffer> ContextGLES::CreateCommandBuffer() const {
  return std::shared_ptr<CommandBufferGLES>(
      new CommandBufferGLES(weak_from_this(), reactor_));
}

// |Context|
const std::shared_ptr<const Capabilities>& ContextGLES::GetCapabilities()
    const {
  return device_capabilities_;
}

// |Context|
std::shared_ptr<CommandQueue> ContextGLES::GetCommandQueue() const {
  return command_queue_;
}

// |Context|
void ContextGLES::ResetThreadLocalState() const {
  if (!IsValid()) {
    return;
  }
  [[maybe_unused]] auto result =
      reactor_->AddOperation([](const ReactorGLES& reactor) {
        RenderPassGLES::ResetGLState(reactor.GetProcTable());
      });
}

bool ContextGLES::EnqueueCommandBuffer(
    std::shared_ptr<CommandBuffer> command_buffer) {
  return command_buffer &&
         GetCommandQueue()
             ->Submit({std::move(command_buffer)}, nullptr, false)
             .ok();
}

// |Context|
[[nodiscard]] bool ContextGLES::FlushCommandBuffers() {
  return reactor_->React();
}

bool ContextGLES::FinishQueue() {
  reactor_->GetProcTable().Finish();
  return true;
}

// |Context|
bool ContextGLES::AddTrackingFence(
    const std::shared_ptr<Texture>& texture) const {
  if (!reactor_->GetProcTable().FenceSync.IsAvailable()) {
    return false;
  }
  HandleGLES fence = reactor_->CreateHandle(HandleType::kFence);
  TextureGLES::Cast(*texture).SetFence(fence);
  return true;
}

// |Context|
RuntimeStageBackend ContextGLES::GetRuntimeStageBackend() const {
  if (GetReactor()->GetProcTable().GetDescription()->GetGlVersion().IsAtLeast(
          Version{3, 0, 0})) {
    return RuntimeStageBackend::kOpenGLES3;
  }
  return RuntimeStageBackend::kOpenGLES;
}

}  // namespace impeller
