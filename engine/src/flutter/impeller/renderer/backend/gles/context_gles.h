// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_CONTEXT_GLES_H_
#define FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_CONTEXT_GLES_H_

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "impeller/base/backend_cast.h"
#include "impeller/core/runtime_types.h"
#include "impeller/renderer/backend/gles/allocator_gles.h"
#include "impeller/renderer/backend/gles/capabilities_gles.h"
#include "impeller/renderer/backend/gles/gpu_tracer_gles.h"
#include "impeller/renderer/backend/gles/pipeline_library_gles.h"
#include "impeller/renderer/backend/gles/reactor_gles.h"
#include "impeller/renderer/backend/gles/sampler_library_gles.h"
#include "impeller/renderer/backend/gles/shader_library_gles.h"
#include "impeller/renderer/capabilities.h"
#include "impeller/renderer/command_queue.h"
#include "impeller/renderer/context.h"

namespace impeller {

class ContextGLES final : public Context,
                          public BackendCast<ContextGLES, Context>,
                          public std::enable_shared_from_this<ContextGLES> {
 public:
  struct ExternalTarget {
    GLuint texture = 0;
    GLuint framebuffer = 0;
    ISize size;
  };

  enum class ExternalTargetRetirement { kSuccess, kUnknown, kInUse, kFailed };

  // Membership operations run with the raster context current. The host keeps
  // the texture alive until retirement succeeds. Acquisitions retain the entry
  // through every queued source reader; numeric GL names are never identities.
  uint64_t RegisterExternalTarget(GLuint texture, ISize size);
  std::shared_ptr<const ExternalTarget> AcquireExternalTarget(uint64_t id,
                                                              GLuint texture,
                                                              ISize size) const;
  ExternalTargetRetirement RetireExternalTarget(uint64_t id);
  bool RetireExternalRenderTargets() override;

  static std::shared_ptr<ContextGLES> Create(
      const Flags& flags,
      std::unique_ptr<ProcTableGLES> gl,
      const std::vector<std::shared_ptr<fml::Mapping>>& shader_libraries,
      bool enable_gpu_tracing,
      std::shared_ptr<fml::BasicTaskRunner> io_task_runner = nullptr,
      std::optional<AvioAntialiasingConfig> antialiasing_config = std::nullopt);

  // |Context|
  ~ContextGLES() override;

  // |Context|
  BackendType GetBackendType() const override;

  const std::shared_ptr<ReactorGLES>& GetReactor() const;

  const AvioAntialiasingConfig& GetAvioAntialiasingConfig() const override;
  AvioRenderResourceReport GetAvioRenderResourceReport(
      bool reset_interval) const override;

  std::optional<ReactorGLES::WorkerID> AddReactorWorker(
      const std::shared_ptr<ReactorGLES::Worker>& worker);

  bool RemoveReactorWorker(ReactorGLES::WorkerID id);

  std::shared_ptr<GPUTracerGLES> GetGPUTracer() const { return gpu_tracer_; }

  // Mutable tracker for command buffer submission bookkeeping.
  const std::shared_ptr<GpuSubmissionTracker>& GetMutableSubmissionTracker()
      const;

  /// A selected external target lost its native completion dependency. Reactor
  /// consumption cannot turn this into a proof that native resources are idle.
  void MarkExternalCompletionUnobservable();

  bool IsSafeToDestroyNativeResources() const override;

 private:
  std::shared_ptr<ReactorGLES> reactor_;
  std::shared_ptr<GpuSubmissionTracker> submission_tracker_ =
      std::make_shared<GpuSubmissionTracker>();
  std::shared_ptr<ShaderLibraryGLES> shader_library_;
  std::shared_ptr<PipelineLibraryGLES> pipeline_library_;
  std::shared_ptr<SamplerLibraryGLES> sampler_library_;
  std::shared_ptr<AllocatorGLES> resource_allocator_;
  std::shared_ptr<CommandQueue> command_queue_;
  std::atomic_bool external_completion_unobservable_ = false;
  mutable std::mutex external_targets_mutex_;
  std::optional<std::thread::id> external_target_owner_;
  mutable size_t external_targets_created_ = 0;
  mutable size_t external_targets_retired_ = 0;
  std::unordered_map<uint64_t, std::shared_ptr<ExternalTarget>>
      external_targets_;
  std::shared_ptr<GPUTracerGLES> gpu_tracer_;

  // Note: This is stored separately from the ProcTableGLES CapabilitiesGLES
  // in order to satisfy the Context::GetCapabilities signature which returns
  // a reference.
  std::shared_ptr<const Capabilities> device_capabilities_;
  bool is_valid_ = false;
  const AvioAntialiasingConfig antialiasing_config_;

  ContextGLES(
      const Flags& flags,
      std::unique_ptr<ProcTableGLES> gl,
      const std::vector<std::shared_ptr<fml::Mapping>>& shader_libraries,
      bool enable_gpu_tracing,
      std::shared_ptr<fml::BasicTaskRunner> io_task_runner = nullptr,
      std::optional<AvioAntialiasingConfig> antialiasing_config = std::nullopt);

  // |Context|
  std::string DescribeGpuModel() const override;

  // |Context|
  bool IsValid() const override;

  // |Context|
  std::shared_ptr<Allocator> GetResourceAllocator() const override;

  // |Context|
  std::shared_ptr<const GpuSubmissionTracker> GetSubmissionTracker()
      const override;

  // |Context|
  std::shared_ptr<ShaderLibrary> GetShaderLibrary() const override;

  // |Context|
  std::shared_ptr<SamplerLibrary> GetSamplerLibrary() const override;

  // |Context|
  std::shared_ptr<PipelineLibrary> GetPipelineLibrary() const override;

  // |Context|
  std::shared_ptr<CommandBuffer> CreateCommandBuffer() const override;

  // |Context|
  const std::shared_ptr<const Capabilities>& GetCapabilities() const override;

  // |Context|
  std::shared_ptr<CommandQueue> GetCommandQueue() const override;

  // |Context|
  void Shutdown() override;

  // |Context|
  bool AddTrackingFence(const std::shared_ptr<Texture>& texture) const override;

  // |Context|
  void ResetThreadLocalState() const override;

  // |Context|
  [[nodiscard]] bool EnqueueCommandBuffer(
      std::shared_ptr<CommandBuffer> command_buffer) override;

  // |Context|
  [[nodiscard]] bool FlushCommandBuffers() override;

  // |Context|
  [[nodiscard]] bool FinishQueue() override;

  // |Context|
  RuntimeStageBackend GetRuntimeStageBackend() const override;

  ContextGLES(const ContextGLES&) = delete;

  ContextGLES& operator=(const ContextGLES&) = delete;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_CONTEXT_GLES_H_
