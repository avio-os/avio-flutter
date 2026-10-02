// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/vulkan/timeline_completion_vk.h"
#include "impeller/renderer/native_teardown_status.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <type_traits>
#include <vector>

#include "flutter/fml/concurrent_message_loop.h"
#include "gtest/gtest.h"

namespace impeller {
namespace testing {
namespace {
// Tests replace only Vulkan dispatch. The completion actor, reservation table,
// real Vulkan handles, callback separation and native holder are production
// code.
struct NativeTimeline {
  std::mutex mutex;
  std::condition_variable cv;
  uint64_t completed = 0u;
  VkResult wait_result = VK_SUCCESS;
  VkResult query_result = VK_SUCCESS;
  std::atomic_size_t destroyed_semaphores = 0u;
};
NativeTimeline* Native(VkDevice device) {
  return reinterpret_cast<NativeTimeline*>(device);
}
VkResult VKAPI_CALL CreateSemaphore(VkDevice,
                                    const VkSemaphoreCreateInfo*,
                                    const VkAllocationCallbacks*,
                                    VkSemaphore* result) {
  *result = reinterpret_cast<VkSemaphore>(uintptr_t{1});
  return VK_SUCCESS;
}
void VKAPI_CALL DestroySemaphore(VkDevice device,
                                 VkSemaphore,
                                 const VkAllocationCallbacks*) {
  Native(device)->destroyed_semaphores++;
}
VkResult VKAPI_CALL WaitSemaphores(VkDevice device,
                                   const VkSemaphoreWaitInfo* info,
                                   uint64_t timeout) {
  auto& native = *Native(device);
  std::unique_lock lock(native.mutex);
  native.cv.wait_for(lock, std::chrono::nanoseconds(timeout), [&] {
    return native.wait_result != VK_SUCCESS ||
           native.completed >= info->pValues[0];
  });
  if (native.wait_result != VK_SUCCESS) {
    return native.wait_result;
  }
  return native.completed >= info->pValues[0] ? VK_SUCCESS : VK_TIMEOUT;
}
VkResult VKAPI_CALL GetCounter(VkDevice device, VkSemaphore, uint64_t* value) {
  auto& native = *Native(device);
  std::scoped_lock lock(native.mutex);
  *value = native.completed;
  return native.query_result;
}
class NativeDeviceHolder final : public DeviceHolderVK {
 public:
  explicit NativeDeviceHolder(NativeTimeline& native)
      : device_(reinterpret_cast<VkDevice>(&native)) {}
  const vk::Device& GetDevice() const override { return device_; }
  const vk::PhysicalDevice& GetPhysicalDevice() const override {
    return physical_;
  }

 private:
  vk::Device device_;
  vk::PhysicalDevice physical_;
};
class TimelineCustodyVKTest : public ::testing::Test {
 protected:
  void SetUp() override {
    prior_dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateSemaphore = CreateSemaphore;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroySemaphore = DestroySemaphore;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitSemaphoresKHR = WaitSemaphores;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetSemaphoreCounterValueKHR = GetCounter;
    holder = std::make_shared<NativeDeviceHolder>(native);
    completion = std::make_shared<TimelineCompletionVK>(holder);
    ASSERT_TRUE(completion->IsValid());
  }
  void TearDown() override {
    if (completion) {
      completion->CompleteDeviceTeardown(vk::Result::eSuccess);
    }
    completion.reset();
    holder.reset();
    EXPECT_EQ(native.destroyed_semaphores, 1u);
    VULKAN_HPP_DEFAULT_DISPATCHER = prior_dispatch;
  }
  void Complete(uint64_t value) {
    {
      std::scoped_lock lock(native.mutex);
      native.completed = value;
    }
    native.cv.notify_all();
  }
  void FailWait() {
    {
      std::scoped_lock lock(native.mutex);
      native.wait_result = VK_ERROR_UNKNOWN;
    }
    native.cv.notify_all();
  }
  bool WaitUntil(const std::function<bool()>& predicate) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    return predicate();
  }
  NativeTimeline native;
  std::remove_reference_t<decltype(VULKAN_HPP_DEFAULT_DISPATCHER)>
      prior_dispatch;
  std::shared_ptr<NativeDeviceHolder> holder;
  std::shared_ptr<TimelineCompletionVK> completion;
};

TEST_F(TimelineCustodyVKTest,
       NativeOwnersRetireBeforeOneSuccessfulNotification) {
  auto resource = std::make_shared<int>(7);
  std::weak_ptr<int> witness = resource;
  const auto reservation = completion->ReserveCompletion();
  ASSERT_TRUE(reservation);
  const auto value = completion->ReserveSubmitValue();
  std::atomic_size_t notifications = 0u;
  ASSERT_TRUE(completion->AddSubmittedCompletion(
      *reservation, value,
      [resource = std::move(resource)]() mutable { resource.reset(); },
      [&](CommandBuffer::Status status) {
        EXPECT_EQ(status, CommandBuffer::Status::kCompleted);
        EXPECT_TRUE(witness.expired());
        notifications++;
      }));
  EXPECT_FALSE(witness.expired());
  Complete(value);
  ASSERT_TRUE(WaitUntil([&] { return notifications == 1u; }));
  completion->CompleteDeviceTeardown(vk::Result::eSuccess);
  EXPECT_EQ(notifications, 1u);
}

TEST_F(TimelineCustodyVKTest, RejectionAfterSubmitRetainsOwnersUntilIdleProof) {
  const auto reservation = completion->ReserveCompletion();
  ASSERT_TRUE(reservation);
  const auto value = completion->ReserveSubmitValue();
  auto resource = std::make_shared<int>(9);
  std::weak_ptr<int> witness = resource;
  size_t notifications = 0u;
  completion->StopAccepting();
  EXPECT_FALSE(completion->AddSubmittedCompletion(
      *reservation, value,
      [resource = std::move(resource)]() mutable { resource.reset(); },
      [&](CommandBuffer::Status status) {
        EXPECT_EQ(status, CommandBuffer::Status::kError);
        notifications++;
      }));
  EXPECT_EQ(notifications, 1u);
  EXPECT_FALSE(witness.expired());
  EXPECT_FALSE(completion->ReserveCompletion());
  EXPECT_TRUE(completion->CompleteDeviceTeardown(vk::Result::eSuccess));
  EXPECT_TRUE(witness.expired());
  EXPECT_EQ(notifications, 1u);
}

TEST_F(TimelineCustodyVKTest, UnknownWaitAndIdleNeverReleaseSubmittedOwners) {
  const auto reservation = completion->ReserveCompletion();
  ASSERT_TRUE(reservation);
  const auto value = completion->ReserveSubmitValue();
  auto resource = std::make_shared<int>(11);
  std::weak_ptr<int> witness = resource;
  std::atomic_size_t notifications = 0u;
  ASSERT_TRUE(completion->AddSubmittedCompletion(
      *reservation, value,
      [resource = std::move(resource)]() mutable { resource.reset(); },
      [&](CommandBuffer::Status status) {
        EXPECT_EQ(status, CommandBuffer::Status::kError);
        notifications++;
      }));
  FailWait();
  ASSERT_TRUE(WaitUntil([&] { return notifications == 1u; }));
  EXPECT_FALSE(completion->IsValid());
  EXPECT_FALSE(witness.expired());
  EXPECT_FALSE(completion->CompleteDeviceTeardown(vk::Result::eErrorUnknown));
  EXPECT_FALSE(witness.expired());
  auto replacement = std::make_shared<TimelineCompletionVK>(holder);
  EXPECT_FALSE(replacement->IsValid());
  replacement.reset();
  EXPECT_EQ(notifications, 1u);
  EXPECT_TRUE(completion->CompleteDeviceTeardown(vk::Result::eErrorDeviceLost));
  EXPECT_TRUE(witness.expired());
  EXPECT_EQ(notifications, 1u);
}

TEST_F(TimelineCustodyVKTest,
       QueryFailurePreservesOtherReadersAfterProvenWait) {
  const auto first = completion->ReserveCompletion();
  const auto second = completion->ReserveCompletion();
  ASSERT_TRUE(first && second);
  const auto value1 = completion->ReserveSubmitValue();
  const auto value2 = completion->ReserveSubmitValue();
  auto resource1 = std::make_shared<int>(1);
  auto resource2 = std::make_shared<int>(2);
  std::weak_ptr<int> witness1 = resource1, witness2 = resource2;
  std::atomic_size_t completed = 0u, failed = 0u;
  ASSERT_TRUE(completion->AddSubmittedCompletion(
      *first, value1,
      [resource1 = std::move(resource1)]() mutable { resource1.reset(); },
      [&](CommandBuffer::Status status) {
        EXPECT_EQ(status, CommandBuffer::Status::kCompleted);
        completed++;
      }));
  ASSERT_TRUE(completion->AddSubmittedCompletion(
      *second, value2,
      [resource2 = std::move(resource2)]() mutable { resource2.reset(); },
      [&](CommandBuffer::Status status) {
        EXPECT_EQ(status, CommandBuffer::Status::kError);
        failed++;
      }));
  {
    std::scoped_lock lock(native.mutex);
    native.query_result = VK_ERROR_UNKNOWN;
  }
  Complete(value1);
  ASSERT_TRUE(WaitUntil([&] { return completed == 1u && failed == 1u; }));
  EXPECT_TRUE(witness1.expired());
  EXPECT_FALSE(witness2.expired());
  completion->CompleteDeviceTeardown(vk::Result::eSuccess);
  EXPECT_TRUE(witness2.expired());
  EXPECT_EQ(completed, 1u);
  EXPECT_EQ(failed, 1u);
}

TEST_F(TimelineCustodyVKTest, CapacityIsReservedBeforeSubmitAndReuseIsExact) {
  std::array<uint64_t, TimelineCompletionVK::kMaxPendingCompletions> held;
  for (auto& reservation : held) {
    auto next = completion->ReserveCompletion();
    ASSERT_TRUE(next);
    reservation = *next;
  }
  EXPECT_FALSE(completion->ReserveCompletion());
  completion->CancelCompletion(held[0]);
  const auto successor = completion->ReserveCompletion();
  ASSERT_TRUE(successor);
  EXPECT_NE(*successor, held[0]);
  completion->CancelCompletion(held[0]);
  EXPECT_TRUE(completion->CanSubmit(*successor));
  EXPECT_FALSE(completion->ReserveCompletion());
  completion->CancelCompletion(*successor);
  for (size_t i = 1; i < held.size(); i++) {
    completion->CancelCompletion(held[i]);
  }
}

TEST_F(TimelineCustodyVKTest, LateRegistrationUsesActualIdleProof) {
  const auto reservation = completion->ReserveCompletion();
  ASSERT_TRUE(reservation);
  const auto value = completion->ReserveSubmitValue();
  auto resource = std::make_shared<int>(17);
  std::weak_ptr<int> witness = resource;
  EXPECT_TRUE(completion->CompleteDeviceTeardown(vk::Result::eSuccess));
  size_t notifications = 0u;
  EXPECT_TRUE(completion->AddSubmittedCompletion(
      *reservation, value,
      [resource = std::move(resource)]() mutable { resource.reset(); },
      [&](CommandBuffer::Status status) {
        EXPECT_EQ(status, CommandBuffer::Status::kCompleted);
        EXPECT_TRUE(witness.expired());
        notifications++;
      }));
  EXPECT_TRUE(witness.expired());
  EXPECT_EQ(notifications, 1u);
}

TEST_F(TimelineCustodyVKTest,
       UnknownTeardownReceiptCannotBecomeCollectedSuccess) {
  NativeTeardownStatus host_receipt;
  EXPECT_FALSE(host_receipt.RecordProof(
      completion->CompleteDeviceTeardown(vk::Result::eErrorUnknown)));
  // CollectShell removes the public Context. A later deinitialize sees no
  // Context to query; that absence must not release the host's borrowed device.
  EXPECT_FALSE(host_receipt.RecordProof(true));
  // Even a later actor proof cannot revive this terminal host shutdown attempt.
  EXPECT_TRUE(completion->CompleteDeviceTeardown(vk::Result::eSuccess));
  EXPECT_FALSE(host_receipt.RecordProof(true));
}

TEST_F(TimelineCustodyVKTest,
       ProvenTeardownReceiptPermitsBorrowedDeviceRelease) {
  NativeTeardownStatus host_receipt;
  EXPECT_TRUE(host_receipt.RecordProof(
      completion->CompleteDeviceTeardown(vk::Result::eSuccess)));
  EXPECT_TRUE(host_receipt.RecordProof(true));
}

TEST_F(TimelineCustodyVKTest, DeviceLostProofPermitsTerminalNativeDestruction) {
  NativeTeardownStatus host_receipt;
  EXPECT_TRUE(host_receipt.RecordProof(
      completion->CompleteDeviceTeardown(vk::Result::eErrorDeviceLost)));
}

TEST_F(TimelineCustodyVKTest, NotificationCanSynchronouslyTearDownContext) {
  const auto reservation = completion->ReserveCompletion();
  ASSERT_TRUE(reservation);
  const auto value = completion->ReserveSubmitValue();
  std::atomic_size_t notifications = 0u;
  ASSERT_TRUE(completion->AddSubmittedCompletion(
      *reservation, value, [] {},
      [&](CommandBuffer::Status status) {
        EXPECT_EQ(status, CommandBuffer::Status::kCompleted);
        EXPECT_TRUE(completion->CompleteDeviceTeardown(vk::Result::eSuccess));
        notifications++;
      }));
  Complete(value);
  ASSERT_TRUE(WaitUntil([&] { return notifications == 1u; }));
  // This cold call joins the waiter if the callback stopped it on itself.
  EXPECT_TRUE(completion->CompleteDeviceTeardown(vk::Result::eSuccess));
  EXPECT_EQ(notifications, 1u);
}

TEST_F(TimelineCustodyVKTest, FinalNotificationMayDropPublicActor) {
  const auto reservation = completion->ReserveCompletion();
  ASSERT_TRUE(reservation);
  const auto value = completion->ReserveSubmitValue();
  std::weak_ptr<TimelineCompletionVK> witness = completion;
  std::atomic_bool notified = false;
  ASSERT_TRUE(completion->AddSubmittedCompletion(
      *reservation, value, [] {},
      [&](CommandBuffer::Status status) {
        EXPECT_EQ(status, CommandBuffer::Status::kCompleted);
        auto actor = std::move(completion);
        EXPECT_TRUE(actor->CompleteDeviceTeardown(vk::Result::eSuccess));
        actor.reset();
        notified = true;
      }));
  Complete(value);
  ASSERT_TRUE(WaitUntil([&] {
    return notified && witness.expired() && native.destroyed_semaphores == 1u;
  }));
  EXPECT_FALSE(completion);
}

TEST_F(TimelineCustodyVKTest, WorkerShutdownRefusesSelfJoinAndPinsExactLoop) {
  auto workers = fml::ConcurrentMessageLoop::Create(1u);
  std::array<std::shared_ptr<void>, 7> dependencies;
  dependencies.back() = workers;
  completion->SetTeardownDependencies(std::move(dependencies));
  std::weak_ptr<fml::ConcurrentMessageLoop> weak_workers = workers;
  std::mutex done_mutex;
  std::condition_variable done_cv;
  bool done = false;
  workers->GetTaskRunner()->PostTask([&] {
    EXPECT_FALSE(workers->TerminateAndJoin());
    EXPECT_FALSE(completion->CompleteDeviceTeardown(vk::Result::eErrorUnknown));
    {
      std::scoped_lock lock(done_mutex);
      done = true;
    }
    done_cv.notify_one();
  });
  {
    std::unique_lock lock(done_mutex);
    ASSERT_TRUE(
        done_cv.wait_for(lock, std::chrono::seconds(2), [&] { return done; }));
  }
  workers.reset();
  completion->Terminate();
  EXPECT_FALSE(weak_workers.expired());
  // The off-worker cold edge can join the same retained loop. Only then is
  // genuine device proof allowed to release its last native dependency.
  auto retained = weak_workers.lock();
  ASSERT_TRUE(retained);
  EXPECT_TRUE(retained->TerminateAndJoin());
  EXPECT_TRUE(retained->TerminateAndJoin());
  retained.reset();
  EXPECT_TRUE(completion->CompleteDeviceTeardown(vk::Result::eSuccess));
  completion.reset();
  EXPECT_TRUE(weak_workers.expired());
}

TEST_F(TimelineCustodyVKTest, MissingShellCannotReplaceFailedStartupProof) {
  NativeTeardownStatus startup_receipt;
  EXPECT_FALSE(startup_receipt.RecordProof(
      completion->CompleteDeviceTeardown(vk::Result::eErrorUnknown)));
  // A failed factory/LaunchShell has no platform-view context to query. Its
  // absent Shell must preserve the exact already-observed startup failure.
  EXPECT_FALSE(startup_receipt.RecordProof(true));
  EXPECT_FALSE(completion->IsValid());
  EXPECT_TRUE(completion->CompleteDeviceTeardown(vk::Result::eErrorDeviceLost));
}

TEST_F(TimelineCustodyVKTest, ColdContextRegistryRefusesUnboundedGrowth) {
  std::vector<std::shared_ptr<TimelineCompletionVK>> actors;
  for (size_t i = 1; i < TimelineCompletionVK::kMaxContexts; i++) {
    auto actor = std::make_shared<TimelineCompletionVK>(holder);
    ASSERT_TRUE(actor->IsValid());
    actors.push_back(std::move(actor));
  }
  auto refused = std::make_shared<TimelineCompletionVK>(holder);
  EXPECT_FALSE(refused->IsValid());
  EXPECT_FALSE(refused->ReserveCompletion());
  refused.reset();
  actors.clear();
  // Only the main fixture's semaphore remains live.
  EXPECT_EQ(native.destroyed_semaphores,
            TimelineCompletionVK::kMaxContexts - 1u);
  native.destroyed_semaphores = 0u;
}

TEST_F(TimelineCustodyVKTest,
       UnknownContextPinsNativeDependenciesAcrossPublicDrop) {
  auto dependencies = std::make_shared<int>(15);
  std::weak_ptr<int> dependency_witness = dependencies;
  completion->SetTeardownDependencies({dependencies, {}, {}, {}, {}, {}});
  dependencies.reset();
  auto resource = std::make_shared<int>(16);
  std::weak_ptr<int> resource_witness = resource;
  auto reservation = completion->ReserveCompletion();
  ASSERT_TRUE(reservation);
  std::atomic_size_t notifications = 0u;
  ASSERT_TRUE(completion->AddSubmittedCompletion(
      *reservation, completion->ReserveSubmitValue(),
      [resource = std::move(resource)]() mutable { resource.reset(); },
      [&](CommandBuffer::Status) { notifications++; }));
  FailWait();
  ASSERT_TRUE(WaitUntil([&] { return notifications == 1u; }));
  std::weak_ptr<TimelineCompletionVK> actor_witness = completion;
  completion.reset();
  EXPECT_FALSE(actor_witness.expired());
  EXPECT_FALSE(resource_witness.expired());
  EXPECT_FALSE(dependency_witness.expired());
  completion = actor_witness.lock();
  completion->CompleteDeviceTeardown(vk::Result::eSuccess);
  EXPECT_TRUE(resource_witness.expired());
  // Cold dependencies live through actual native destruction, not notification.
  EXPECT_FALSE(dependency_witness.expired());
  EXPECT_EQ(notifications, 1u);
}
}  // namespace
}  // namespace testing
}  // namespace impeller
