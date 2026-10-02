// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_TIMELINE_COMPLETION_VK_H_
#define FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_TIMELINE_COMPLETION_VK_H_

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "impeller/renderer/backend/vulkan/device_holder_vk.h"
#include "impeller/renderer/command_buffer.h"

namespace impeller {

// One bounded native completion owner per queue. Unknown completion stops
// submission; notifying an error never proves that submitted storage is
// reusable.
class TimelineCompletionVK
    : public std::enable_shared_from_this<TimelineCompletionVK> {
 public:
  using CompletionCallback = std::function<void(CommandBuffer::Status)>;
  using ResourceCleanup = std::function<void()>;
  static constexpr size_t kMaxPendingCompletions = 128u;
  static constexpr size_t kMaxContexts = 64u;

  explicit TimelineCompletionVK(std::weak_ptr<DeviceHolderVK> device_holder);
  ~TimelineCompletionVK();
  bool IsValid() const;
  uint64_t ReserveSubmitValue();
  vk::Semaphore GetSemaphore() const;

  // Reserve native custody BEFORE queue.submit. Exhaustion refuses that submit;
  // it never creates another heap-backed pending or quarantine entry.
  std::optional<uint64_t> ReserveCompletion();
  bool CanSubmit(uint64_t reservation) const;
  void CancelCompletion(uint64_t reservation);

  // After a successful native submit this always adopts the reserved custody,
  // including a concurrent stop/fault. False means notification was kError;
  // the caller MUST NOT wait, retry submission, or destroy the adopted owners.
  bool AddSubmittedCompletion(uint64_t reservation,
                              uint64_t value,
                              ResourceCleanup release_resources,
                              CompletionCallback callback);

  // Notification-only compatibility helper. Submitted owners use the reserved
  // API above so an admission refusal cannot destroy GPU-referenced storage.
  bool AddCompletion(uint64_t value, CompletionCallback callback);
  bool WaitFor(uint64_t value);
  uint64_t GetCompletedValue() const;

  // Cold context dependencies remain valid during uncertain native retirement.
  void SetTeardownDependencies(std::array<std::shared_ptr<void>, 7> owners);
  void StopAccepting();
  void Terminate();
  // Called only after stopping admission and externally synchronizing queues.
  // Success proves idle; DEVICE_LOST permits lost-device destruction. Any other
  // result retains bounded native custody and blocks replacement contexts.
  bool CompleteDeviceTeardown(vk::Result result);

 private:
  enum class EntryState { kFree, kReserved, kPending, kQuarantined };
  struct Entry {
    EntryState state = EntryState::kFree;
    uint64_t reservation = 0u;
    uint64_t value = 0u;
    ResourceCleanup release_resources;
    CompletionCallback callback;
  };
  // Declared before native semaphore/dependencies: destroyed last.
  std::shared_ptr<DeviceHolderVK> device_holder_;
  vk::UniqueSemaphore timeline_;
  std::array<std::shared_ptr<void>, 7> teardown_dependencies_;
  std::unique_ptr<std::thread> waiter_thread_;
  std::atomic_uint64_t next_value_ = 0u;
  std::atomic_uint64_t completed_value_ = 0u;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::array<Entry, kMaxPendingCompletions> pending_;
  uint64_t next_reservation_ = 0u;
  std::optional<size_t> context_slot_;
  std::optional<vk::Result> teardown_result_;
  bool terminate_ = false;
  bool accepting_ = false;
  bool is_valid_ = false;
  bool unavailable_ = false;

  Entry* FindReserved(uint64_t reservation);
  void Main();
  bool WaitForValue(const vk::Device& device, uint64_t value);
  std::optional<uint64_t> QueryCompletedValue(const vk::Device& device);
  void AdvanceCompletedValue(uint64_t value);
  void DrainReady(uint64_t value, CommandBuffer::Status status);
  void MarkUnavailable();
  void RetainActor(bool uncertain);
  void ReleaseActor();
  TimelineCompletionVK(const TimelineCompletionVK&) = delete;
  TimelineCompletionVK& operator=(const TimelineCompletionVK&) = delete;
};

}  // namespace impeller
#endif  // FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_TIMELINE_COMPLETION_VK_H_
