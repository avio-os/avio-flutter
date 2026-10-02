// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/vulkan/timeline_completion_vk.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "flutter/fml/cpu_affinity.h"
#include "flutter/fml/logging.h"
#include "flutter/fml/thread.h"
#include "flutter/fml/trace_event.h"
#include "impeller/base/validation.h"

namespace impeller {
namespace {
struct ContextCustody {
  bool occupied = false;
  bool uncertain = false;
  std::shared_ptr<TimelineCompletionVK> actor;
};
struct CompletionRegistry {
  std::mutex mutex;
  std::array<ContextCustody, TimelineCompletionVK::kMaxContexts> contexts;
};
CompletionRegistry& Registry() {
  // Cold, fixed process-lifetime storage. C++ static destruction must not turn
  // process exit into a false native completion proof for quarantined devices.
  static auto* registry = new CompletionRegistry();
  return *registry;
}
}  // namespace

TimelineCompletionVK::TimelineCompletionVK(
    std::weak_ptr<DeviceHolderVK> device_holder)
    : device_holder_(device_holder.lock()) {
  if (!device_holder_) {
    return;
  }
  auto& registry = Registry();
  {
    std::scoped_lock lock(registry.mutex);
    if (std::any_of(registry.contexts.begin(), registry.contexts.end(),
                    [](const auto& entry) { return entry.uncertain; })) {
      return;
    }
    for (size_t i = 0; i < registry.contexts.size(); i++) {
      if (!registry.contexts[i].occupied) {
        registry.contexts[i].occupied = true;
        context_slot_ = i;
        break;
      }
    }
  }
  if (!context_slot_) {
    return;
  }
  vk::StructureChain<vk::SemaphoreCreateInfo, vk::SemaphoreTypeCreateInfo>
      chain;
  auto& type = chain.get<vk::SemaphoreTypeCreateInfo>();
  type.setSemaphoreType(vk::SemaphoreType::eTimeline);
  type.setInitialValue(0u);
  auto [result, semaphore] =
      device_holder_->GetDevice().createSemaphoreUnique(chain.get());
  if (result != vk::Result::eSuccess) {
    VALIDATION_LOG << "Could not create Vulkan timeline semaphore: "
                   << vk::to_string(result);
    return;
  }
  timeline_ = std::move(semaphore);
  accepting_ = is_valid_ = true;
  waiter_thread_ = std::make_unique<std::thread>([this]() { Main(); });
}

TimelineCompletionVK::~TimelineCompletionVK() {
  Terminate();
  if (waiter_thread_ && waiter_thread_->joinable() &&
      waiter_thread_->get_id() == std::this_thread::get_id()) {
    // Main's final local owner can be the last owner after a callback shuts
    // down its context. Main has finished using this object at this point.
    waiter_thread_->detach();
  }
  // A submitted actor is anchored in its cold registry slot until a context
  // supplies safe teardown proof, so its unproven cleanup cannot arrive here.
  for (const auto& entry : pending_) {
    FML_CHECK(!entry.release_resources);
  }
  if (context_slot_) {
    auto& registry = Registry();
    std::scoped_lock lock(registry.mutex);
    auto& slot = registry.contexts[*context_slot_];
    FML_CHECK(!slot.actor);
    slot.occupied = slot.uncertain = false;
  }
}

bool TimelineCompletionVK::IsValid() const {
  std::scoped_lock lock(mutex_);
  return is_valid_ && !!timeline_ && accepting_ && !unavailable_ && !terminate_;
}
uint64_t TimelineCompletionVK::ReserveSubmitValue() {
  return next_value_.fetch_add(1u, std::memory_order_relaxed) + 1u;
}
vk::Semaphore TimelineCompletionVK::GetSemaphore() const {
  return timeline_.get();
}
std::optional<uint64_t> TimelineCompletionVK::ReserveCompletion() {
  std::scoped_lock lock(mutex_);
  if (!accepting_ || unavailable_ || terminate_ || !is_valid_ ||
      next_reservation_ == std::numeric_limits<uint64_t>::max()) {
    return std::nullopt;
  }
  for (auto& entry : pending_) {
    if (entry.state == EntryState::kFree) {
      entry.state = EntryState::kReserved;
      entry.reservation = ++next_reservation_;
      return entry.reservation;
    }
  }
  return std::nullopt;
}
TimelineCompletionVK::Entry* TimelineCompletionVK::FindReserved(uint64_t id) {
  for (auto& entry : pending_) {
    if (entry.state == EntryState::kReserved && entry.reservation == id) {
      return &entry;
    }
  }
  return nullptr;
}
bool TimelineCompletionVK::CanSubmit(uint64_t reservation) const {
  std::scoped_lock lock(mutex_);
  return accepting_ && !unavailable_ && !terminate_ &&
         std::any_of(pending_.begin(), pending_.end(), [&](const auto& entry) {
           return entry.state == EntryState::kReserved &&
                  entry.reservation == reservation;
         });
}
void TimelineCompletionVK::CancelCompletion(uint64_t reservation) {
  std::scoped_lock lock(mutex_);
  if (auto* entry = FindReserved(reservation)) {
    *entry = {};
  }
}
bool TimelineCompletionVK::AddSubmittedCompletion(
    uint64_t reservation,
    uint64_t value,
    ResourceCleanup release_resources,
    CompletionCallback callback) {
  FML_CHECK(value != 0u);
  const bool native_owners = !!release_resources;
  if (native_owners) {
    RetainActor(false);
  }
  ResourceCleanup ready_resources;
  CompletionCallback ready_callback;
  auto status = CommandBuffer::Status::kCompleted;
  bool accepted = true;
  bool safe_teardown = false;
  {
    std::scoped_lock lock(mutex_);
    auto* entry = FindReserved(reservation);
    // A successfully submitted owner must have secured this exact slot before
    // queue.submit. No stop/fault path cancels a caller's reservation.
    FML_CHECK(entry);
    const bool idle = teardown_result_ == vk::Result::eSuccess;
    const bool lost = teardown_result_ == vk::Result::eErrorDeviceLost;
    safe_teardown = idle || lost;
    const bool completed = value <= completed_value_.load();
    if (idle || lost || completed) {
      ready_resources = std::move(release_resources);
      ready_callback = std::move(callback);
      status = lost ? CommandBuffer::Status::kError : status;
      accepted = !lost;
      *entry = {};
    } else {
      entry->value = value;
      entry->release_resources = std::move(release_resources);
      entry->callback = std::move(callback);
      if (!accepting_ || unavailable_ || terminate_) {
        entry->state = EntryState::kQuarantined;
        ready_callback = std::move(entry->callback);
        status = CommandBuffer::Status::kError;
        accepted = false;
      } else {
        entry->state = EntryState::kPending;
      }
    }
  }
  if (ready_resources) {
    ready_resources();
  }
  if (ready_callback) {
    ready_callback(status);
  }
  if (safe_teardown && native_owners) {
    ReleaseActor();
  }
  cv_.notify_one();
  return accepted;
}
bool TimelineCompletionVK::AddCompletion(uint64_t value,
                                         CompletionCallback callback) {
  if (!callback || value == 0u) {
    return false;
  }
  auto reservation = ReserveCompletion();
  return reservation &&
         AddSubmittedCompletion(*reservation, value, {}, std::move(callback));
}
void TimelineCompletionVK::SetTeardownDependencies(
    std::array<std::shared_ptr<void>, 7> owners) {
  std::scoped_lock lock(mutex_);
  teardown_dependencies_ = std::move(owners);
}
void TimelineCompletionVK::StopAccepting() {
  std::scoped_lock lock(mutex_);
  accepting_ = false;
}
bool TimelineCompletionVK::WaitFor(uint64_t value) {
  if (value <= GetCompletedValue()) {
    return true;
  }
  if (!IsValid() || !WaitForValue(device_holder_->GetDevice(), value)) {
    return false;
  }
  // A successful wait itself proves at least value, even if the query fails.
  auto completed = QueryCompletedValue(device_holder_->GetDevice());
  AdvanceCompletedValue(std::max(value, completed.value_or(value)));
  DrainReady(GetCompletedValue(), CommandBuffer::Status::kCompleted);
  if (!completed) {
    MarkUnavailable();
  }
  return true;
}
uint64_t TimelineCompletionVK::GetCompletedValue() const {
  return completed_value_.load(std::memory_order_acquire);
}
void TimelineCompletionVK::AdvanceCompletedValue(uint64_t value) {
  auto old = completed_value_.load();
  while (old < value && !completed_value_.compare_exchange_weak(old, value)) {
  }
}
void TimelineCompletionVK::Terminate() {
  {
    std::scoped_lock lock(mutex_);
    accepting_ = false;
    terminate_ = true;
  }
  cv_.notify_all();
  if (waiter_thread_ && waiter_thread_->joinable()) {
    if (waiter_thread_->get_id() != std::this_thread::get_id()) {
      waiter_thread_->join();
    }
    // A notification may synchronously shut down its context. Main keeps
    // itself alive through that notification and exits on terminate_ below.
    // Leave its thread joinable for a later cold caller; destruction on the
    // exiting waiter itself detaches the already-finished execution above.
  }
}
void TimelineCompletionVK::Main() {
  fml::Thread::SetCurrentThreadName(
      fml::Thread::ThreadConfig{"IplrVkTimeline"});
  fml::RequestAffinity(fml::CpuAffinity::kEfficiency);
  std::shared_ptr<TimelineCompletionVK> notification_owner;
  while (true) {
    uint64_t target = 0u;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] {
        if (terminate_ || unavailable_) {
          return true;
        }
        return std::any_of(pending_.begin(), pending_.end(), [](const auto& e) {
          return e.state == EntryState::kPending;
        });
      });
      if (terminate_ || unavailable_) {
        break;
      }
      // Constructor startup need not have shared ownership yet; submitted
      // entries do. Keep it through callbacks which may destroy the context.
      notification_owner = weak_from_this().lock();
      FML_CHECK(notification_owner);
      for (const auto& entry : pending_) {
        if (entry.state == EntryState::kPending &&
            (target == 0u || entry.value < target)) {
          target = entry.value;
        }
      }
    }
    if (!WaitForValue(device_holder_->GetDevice(), target)) {
      break;
    }
    auto completed = QueryCompletedValue(device_holder_->GetDevice());
    AdvanceCompletedValue(std::max(target, completed.value_or(target)));
    DrainReady(GetCompletedValue(), CommandBuffer::Status::kCompleted);
    if (!completed) {
      MarkUnavailable();
      break;
    }
  }
  // Stopping or losing observability is NOT permission to release owners.
}
bool TimelineCompletionVK::WaitForValue(const vk::Device& device,
                                        uint64_t value) {
  std::array<vk::Semaphore, 1> semaphores = {timeline_.get()};
  std::array<uint64_t, 1> values = {value};
  vk::SemaphoreWaitInfo info;
  info.setSemaphores(semaphores);
  info.setValues(values);
  TRACE_EVENT0("impeller", "TimelineCompletionVK::WaitForValue");
  while (value > GetCompletedValue()) {
    // Only the existing cold completion actor/public cold wait performs a GPU
    // wait. Finite waits allow shutdown to join after an unobservable device.
    const auto result = device.waitSemaphoresKHR(info, 100'000'000u);
    if (result == vk::Result::eSuccess) {
      return true;
    }
    if (result != vk::Result::eTimeout) {
      VALIDATION_LOG << "Vulkan timeline wait failed: "
                     << vk::to_string(result);
      MarkUnavailable();
      return false;
    }
    std::scoped_lock lock(mutex_);
    if (terminate_ || unavailable_) {
      return false;
    }
  }
  return true;
}
std::optional<uint64_t> TimelineCompletionVK::QueryCompletedValue(
    const vk::Device& device) {
  const auto result = device.getSemaphoreCounterValueKHR(timeline_.get());
  if (result.result != vk::Result::eSuccess) {
    VALIDATION_LOG << "Could not query Vulkan timeline value: "
                   << vk::to_string(result.result);
    return std::nullopt;
  }
  return result.value;
}
void TimelineCompletionVK::DrainReady(uint64_t value,
                                      CommandBuffer::Status status) {
  while (true) {
    Entry ready;
    {
      std::scoped_lock lock(mutex_);
      Entry* first = nullptr;
      for (auto& entry : pending_) {
        if ((entry.state == EntryState::kPending ||
             entry.state == EntryState::kQuarantined) &&
            entry.value <= value && (!first || entry.value < first->value)) {
          first = &entry;
        }
      }
      if (!first) {
        return;
      }
      ready = std::move(*first);
      *first = {};
    }
    if (ready.release_resources) {
      ready.release_resources();
    }
    if (ready.callback) {
      ready.callback(status);
    }
  }
}
void TimelineCompletionVK::MarkUnavailable() {
  {
    std::scoped_lock lock(mutex_);
    if (teardown_result_) {
      return;
    }
    RetainActor(true);
    unavailable_ = true;
    accepting_ = false;
    for (auto& entry : pending_) {
      if (entry.state == EntryState::kPending) {
        entry.state = EntryState::kQuarantined;
      }
    }
  }
  cv_.notify_all();
  while (true) {
    CompletionCallback callback;
    {
      std::scoped_lock lock(mutex_);
      for (auto& entry : pending_) {
        if (entry.state == EntryState::kQuarantined && entry.callback) {
          callback = std::move(entry.callback);
          break;
        }
      }
    }
    if (!callback) {
      return;
    }
    callback(CommandBuffer::Status::kError);
  }
}
void TimelineCompletionVK::RetainActor(bool uncertain) {
  auto self = weak_from_this().lock();
  FML_CHECK(self && context_slot_);
  auto& registry = Registry();
  std::scoped_lock lock(registry.mutex);
  auto& slot = registry.contexts[*context_slot_];
  slot.actor = std::move(self);
  slot.uncertain |= uncertain;
}
void TimelineCompletionVK::ReleaseActor() {
  std::shared_ptr<TimelineCompletionVK> retained;
  auto& registry = Registry();
  {
    std::scoped_lock lock(registry.mutex);
    auto& slot = registry.contexts[*context_slot_];
    retained = std::move(slot.actor);
    slot.uncertain = false;
  }
}
bool TimelineCompletionVK::CompleteDeviceTeardown(vk::Result result) {
  StopAccepting();
  Terminate();
  {
    std::scoped_lock lock(mutex_);
    if (teardown_result_) {
      return true;
    }
  }
  if (result != vk::Result::eSuccess &&
      result != vk::Result::eErrorDeviceLost) {
    MarkUnavailable();
    return false;
  }
  if (result == vk::Result::eErrorDeviceLost) {
    MarkUnavailable();
  }
  {
    std::scoped_lock lock(mutex_);
    teardown_result_ = result;
  }
  DrainReady(std::numeric_limits<uint64_t>::max(),
             result == vk::Result::eSuccess ? CommandBuffer::Status::kCompleted
                                            : CommandBuffer::Status::kError);
  if (context_slot_) {
    ReleaseActor();
  }
  return true;
}
}  // namespace impeller
