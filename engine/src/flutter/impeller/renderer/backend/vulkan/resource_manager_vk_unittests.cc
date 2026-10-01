// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <sys/types.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include "fml/closure.h"
#include "fml/synchronization/waitable_event.h"
#include "gtest/gtest.h"
#include "impeller/renderer/backend/vulkan/resource_manager_vk.h"

namespace impeller {
namespace testing {

// While expected to be a singleton per context, the class does not enforce it.
TEST(ResourceManagerVKTest, CreatesANewInstance) {
  auto const a = ResourceManagerVK::Create();
  auto const b = ResourceManagerVK::Create();
  EXPECT_NE(a, b);
}

TEST(ResourceManagerVKTest, ReclaimMovesAResourceAndDestroysIt) {
  auto const manager = ResourceManagerVK::Create();

  auto waiter = fml::AutoResetWaitableEvent();
  auto dead = false;
  auto rattle = fml::ScopedCleanupClosure([&waiter]() { waiter.Signal(); });

  // Not killed immediately.
  EXPECT_FALSE(waiter.IsSignaledForTest());

  {
    auto resource = UniqueResourceVKT<fml::ScopedCleanupClosure>(
        manager, std::move(rattle));
  }

  waiter.Wait();
}

// Regression test for https://github.com/flutter/flutter/issues/134482.
TEST(ResourceManagerVKTest, TerminatesWhenOutOfScope) {
  // Originally, this shared_ptr was never destroyed, and the thread never
  // terminated. This test ensures that the thread terminates when the
  // ResourceManagerVK is out of scope.
  std::weak_ptr<ResourceManagerVK> manager;

  {
    auto shared = ResourceManagerVK::Create();
    manager = shared;
  }

  // The thread should have terminated.
  EXPECT_EQ(manager.lock(), nullptr);
}

TEST(ResourceManagerVKTest, IsThreadSafe) {
  // In a typical app, there is a single ResourceManagerVK per app, shared b/w
  // threads.
  //
  // This test ensures that the ResourceManagerVK is thread-safe.
  std::weak_ptr<ResourceManagerVK> manager;

  {
    auto const manager = ResourceManagerVK::Create();

    // Spawn two threads, and have them both put resources into the manager.
    struct MockResource {};

    std::thread thread1([&manager]() {
      UniqueResourceVKT<MockResource>(manager, MockResource{});
    });

    std::thread thread2([&manager]() {
      UniqueResourceVKT<MockResource>(manager, MockResource{});
    });

    thread1.join();
    thread2.join();
  }

  // The thread should have terminated.
  EXPECT_EQ(manager.lock(), nullptr);
}

// Red before EN46: Flush did not exist, and a released texture was only
// queued for destruction "at some point in the future".
TEST(ResourceManagerVKTest, FlushWaitsForEveryReclaimQueuedBeforeIt) {
  auto const manager = ResourceManagerVK::Create();

  // Hold the manager thread inside the first resource's destructor so the
  // second one is still queued when Flush starts.
  fml::AutoResetWaitableEvent destroying_first;
  fml::AutoResetWaitableEvent release_first;
  std::atomic<int> destroyed = 0;
  auto first = fml::ScopedCleanupClosure([&]() {
    destroying_first.Signal();
    release_first.Wait();
    destroyed++;
  });
  auto second = fml::ScopedCleanupClosure([&]() { destroyed++; });
  {
    auto resource =
        UniqueResourceVKT<fml::ScopedCleanupClosure>(manager, std::move(first));
  }
  destroying_first.Wait();
  {
    auto resource = UniqueResourceVKT<fml::ScopedCleanupClosure>(
        manager, std::move(second));
  }

  std::atomic<bool> flushed = false;
  std::thread flusher([&]() {
    manager->Flush();
    flushed = true;
  });
  // The flush cannot return while either resource is alive.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(flushed.load());
  EXPECT_EQ(destroyed.load(), 0);

  release_first.Signal();
  flusher.join();
  EXPECT_TRUE(flushed.load());
  EXPECT_EQ(destroyed.load(), 2);
}

TEST(ResourceManagerVKTest, FlushOnIdleManagerReturnsAtOnce) {
  auto const manager = ResourceManagerVK::Create();
  manager->Flush();

  fml::AutoResetWaitableEvent destroyed;
  {
    auto resource = UniqueResourceVKT<fml::ScopedCleanupClosure>(
        manager,
        fml::ScopedCleanupClosure([&destroyed]() { destroyed.Signal(); }));
  }
  manager->Flush();
  // Everything queued before the flush is gone when it returns.
  EXPECT_TRUE(destroyed.IsSignaledForTest());
  // A second flush with nothing pending returns at once.
  manager->Flush();
}

}  // namespace testing
}  // namespace impeller
