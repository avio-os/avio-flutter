// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fml/status.h"

#include <array>

#include "flutter/fml/make_copyable.h"
#include "impeller/renderer/backend/vulkan/command_queue_vk.h"

#include "impeller/base/validation.h"
#include "impeller/renderer/backend/vulkan/command_buffer_vk.h"
#include "impeller/renderer/backend/vulkan/context_vk.h"
#include "impeller/renderer/backend/vulkan/swapchain/ahb/external_semaphore_vk.h"
#include "impeller/renderer/backend/vulkan/timeline_completion_vk.h"
#include "impeller/renderer/backend/vulkan/tracked_objects_vk.h"
#include "impeller/renderer/command_buffer.h"

namespace impeller {
namespace {
class CompletionReservation final {
 public:
  CompletionReservation(TimelineCompletionVK& completion, uint64_t token)
      : completion_(completion), token_(token) {}
  ~CompletionReservation() {
    if (active_) {
      completion_.CancelCompletion(token_);
    }
  }
  void Release() { active_ = false; }

 private:
  TimelineCompletionVK& completion_;
  uint64_t token_;
  bool active_ = true;
};
}  // namespace

CommandQueueVK::CommandQueueVK(const std::weak_ptr<ContextVK>& context)
    : context_(context) {}

CommandQueueVK::~CommandQueueVK() = default;

fml::Status CommandQueueVK::Submit(
    const std::vector<std::shared_ptr<CommandBuffer>>& buffers,
    const CompletionCallback& completion_callback,
    bool block_on_schedule) {
  if (buffers.empty()) {
    return fml::Status(fml::StatusCode::kInvalidArgument,
                       "No command buffers provided.");
  }
  // Success or failure, you only get to submit once.
  fml::ScopedCleanupClosure reset([&]() {
    if (completion_callback) {
      completion_callback(CommandBuffer::Status::kError);
    }
  });

  std::vector<vk::CommandBuffer> vk_buffers;
  std::vector<std::shared_ptr<TrackedObjectsVK>> tracked_objects;
  vk_buffers.reserve(buffers.size());
  tracked_objects.reserve(buffers.size());
  for (const std::shared_ptr<CommandBuffer>& buffer : buffers) {
    CommandBufferVK& command_buffer = CommandBufferVK::Cast(*buffer);
    if (!command_buffer.EndCommandBuffer()) {
      return fml::Status(fml::StatusCode::kCancelled,
                         "Failed to end command buffer.");
    }
    vk_buffers.push_back(command_buffer.GetCommandBuffer());
    // This exact native owner follows the submitted timeline, even when the
    // public wrapper and recorder spans are destroyed immediately afterward.
    command_buffer.tracked_objects_->AdoptResourceOwners(
        command_buffer.TakeResourceOwners());
    tracked_objects.push_back(std::move(command_buffer.tracked_objects_));
  }

  auto context = context_.lock();
  if (!context) {
    VALIDATION_LOG << "Device lost.";
    return fml::Status(fml::StatusCode::kCancelled, "Device lost.");
  }
  auto completion = context->GetTimelineCompletion();
  if (!completion || !completion->IsValid()) {
    VALIDATION_LOG << "Timeline completion tracker is not available.";
    return fml::Status(fml::StatusCode::kCancelled,
                       "Timeline completion tracker is not available.");
  }

  const auto reserved = completion->ReserveCompletion();
  if (!reserved) {
    return {fml::StatusCode::kResourceExhausted,
            "Fixed native completion custody capacity unavailable"};
  }
  CompletionReservation reservation(*completion, *reserved);

  // Collect wait semaphores from all tracked objects (e.g. DMA-BUF
  // acquire fences imported as VkSemaphores).
  std::vector<vk::Semaphore> wait_semaphore_handles;
  std::vector<vk::PipelineStageFlags> wait_stage_masks;
  std::vector<WaitSemaphore> wait_semaphores_storage;
  std::vector<TrackedObjectsVK::PendingSignalSemaphoreVK>
      signal_semaphores_storage;
  std::vector<vk::Semaphore> signal_semaphore_handles;
  for (auto& objs : tracked_objects) {
    auto signals = objs->CreateSignalSemaphores(context);
    signal_semaphore_handles.reserve(signal_semaphore_handles.size() +
                                     signals.size());
    signal_semaphores_storage.reserve(signal_semaphores_storage.size() +
                                      signals.size());
    for (auto& signal : signals) {
      signal_semaphore_handles.push_back(signal.semaphore->GetHandle());
      signal_semaphores_storage.push_back(std::move(signal));
    }
  }

  vk::SemaphoreCreateInfo internal_dependency_info;
  auto [dependency_result, internal_dependency_semaphore] =
      context->GetDevice().createSemaphoreUnique(internal_dependency_info);
  if (dependency_result != vk::Result::eSuccess) {
    VALIDATION_LOG << "Could not create Vulkan submit dependency semaphore: "
                   << vk::to_string(dependency_result);
    return fml::Status(fml::StatusCode::kCancelled,
                       "Failed to create submit dependency semaphore.");
  }
  context->SetDebugName(internal_dependency_semaphore.get(),
                        "ImpellerSubmitCompletionDependency");

  vk::SubmitInfo render_submit_info;
  render_submit_info.setCommandBuffers(vk_buffers);
  signal_semaphore_handles.push_back(internal_dependency_semaphore.get());
  render_submit_info.setSignalSemaphores(signal_semaphore_handles);

  std::array<vk::Semaphore, 1> completion_wait_semaphores = {
      internal_dependency_semaphore.get()};
  std::array<vk::PipelineStageFlags, 1> completion_wait_stage_masks = {
      vk::PipelineStageFlagBits::eTopOfPipe};
  std::array<vk::Semaphore, 1> completion_signal_semaphores = {
      completion->GetSemaphore()};
  std::array<uint64_t, 1> completion_wait_values = {0u};
  std::array<uint64_t, 1> completion_signal_values = {0u};
  vk::TimelineSemaphoreSubmitInfo completion_timeline_submit_info;
  completion_timeline_submit_info.setWaitSemaphoreValues(
      completion_wait_values);
  completion_timeline_submit_info.setSignalSemaphoreValues(
      completion_signal_values);

  vk::SubmitInfo completion_submit_info;
  completion_submit_info.setWaitSemaphores(completion_wait_semaphores);
  completion_submit_info.setWaitDstStageMask(completion_wait_stage_masks);
  completion_submit_info.setSignalSemaphores(completion_signal_semaphores);
  completion_submit_info.setPNext(&completion_timeline_submit_info);

  std::array<vk::SubmitInfo, 2> submit_infos = {render_submit_info,
                                                completion_submit_info};

  auto tracker = context->GetMutableSubmissionTracker();
  uint64_t submission_id = 0u;
  uint64_t completion_value = 0u;
  auto status = context->GetGraphicsQueue()->SubmitLocked(
      [&](const vk::Queue& queue) -> vk::Result {
        if (!completion->CanSubmit(*reserved)) {
          return vk::Result::eErrorUnknown;
        }
        // Take producer dependencies at actual submission, rather than while
        // recording. The queue lock orders two readers of one source and also
        // keeps a failed first submit's returned wait ahead of the next reader.
        for (auto& objs : tracked_objects) {
          for (auto& wait : objs->TakeWaitSemaphores()) {
            wait_semaphore_handles.push_back(*wait.semaphore);
            wait_stage_masks.push_back(wait.wait_stage);
            wait_semaphores_storage.push_back(std::move(wait));
          }
        }
        if (!wait_semaphore_handles.empty()) {
          submit_infos[0].setWaitSemaphores(wait_semaphore_handles);
          submit_infos[0].setWaitDstStageMask(wait_stage_masks);
        }
        // Timeline values must reflect actual queue submission order. The
        // marker batch waits on a queue-local binary semaphore signaled by the
        // render batch, so CPU completion cannot run before render execution
        // reaches that signal.
        completion_value = completion->ReserveSubmitValue();
        submission_id = tracker->RecordSubmission();
        completion_signal_values[0] = completion_value;
        const auto result = queue.submit(submit_infos, vk::Fence{});
        if (result != vk::Result::eSuccess) {
          for (auto& wait : wait_semaphores_storage) {
            auto source = wait.source;
            source->ReturnAcquireSemaphoreFromFailedSubmit(std::move(wait));
          }
          wait_semaphores_storage.clear();
        }
        return result;
      });
  if (status != vk::Result::eSuccess) {
    if (submission_id != 0u) {
      tracker->RecordCompletion(submission_id);
    }
    VALIDATION_LOG << "Failed to submit queue: " << vk::to_string(status);
    return fml::Status(fml::StatusCode::kCancelled, "Failed to submit queue: ");
  }

  // From here the native GPU owns this exact submission. The completion
  // tracker must adopt it even if observation becomes unavailable afterward.
  reservation.Release();
  reset.Release();
  for (const auto& signal : signal_semaphores_storage) {
    signal.texture->SetRenderCompleteSyncFD(signal.semaphore->CreateFD());
  }

  // Submit will proceed, call callback with true when it is done and do not
  // call when `reset` is collected.
  auto added_completion = completion->AddSubmittedCompletion(
      *reserved, completion_value,
      fml::MakeCopyable(
          [tracker, submission_id, tracked_objects = std::move(tracked_objects),
           signal_semaphores_storage = std::move(signal_semaphores_storage),
           internal_dependency_semaphore =
               std::move(internal_dependency_semaphore),
           wait_semaphores_storage =
               std::move(wait_semaphores_storage)]() mutable {
            // Ensure tracked objects and semaphores are destructed before
            // calling any final callbacks.
            signal_semaphores_storage.clear();
            internal_dependency_semaphore.reset();
            wait_semaphores_storage.clear();
            tracked_objects.clear();
            tracker->RecordCompletion(submission_id);
          }),
      completion_callback);
  if (!added_completion) {
    return fml::Status(fml::StatusCode::kCancelled,
                       "Failed to add timeline completion.");
  }
  return fml::Status();
}

}  // namespace impeller
