// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/vulkan/resource_manager_vk.h"

#include "flutter/fml/cpu_affinity.h"
#include "flutter/fml/thread.h"
#include "flutter/fml/trace_event.h"
#include "fml/logging.h"

namespace impeller {

std::shared_ptr<ResourceManagerVK> ResourceManagerVK::Create() {
  // It will be tempting to refactor this to create the waiter thread in the
  // static method instead of the constructor. However, that causes the
  // destructor never to be called, and the thread never terminates!
  //
  // See https://github.com/flutter/flutter/issues/134482.
  return std::shared_ptr<ResourceManagerVK>(new ResourceManagerVK());
}

ResourceManagerVK::ResourceManagerVK() : waiter_([&]() { Start(); }) {}

ResourceManagerVK::~ResourceManagerVK() {
  FML_DCHECK(waiter_.get_id() != std::this_thread::get_id())
      << "The ResourceManager being destructed on its own spawned thread is a "
      << "sign that ContextVK was not properly destroyed. A usual fix for this "
      << "is to ensure that ContextVK is shutdown (i.e. context->Shutdown()) "
         "before the ResourceManager is destroyed (i.e. at the end of a test).";
  Terminate();
  waiter_.join();
}

void ResourceManagerVK::Start() {
  // It's possible for Start() to be called when terminating:
  // { ResourceManagerVK::Create(); }
  //
  // ... so no FML_DCHECK here.

  fml::Thread::SetCurrentThreadName(fml::Thread::ThreadConfig{"IplrVkResMgr"});
  // While this code calls destructors it doesn't need to be particularly fast
  // with them, as long as it doesn't interrupt raster thread.
  fml::RequestAffinity(fml::CpuAffinity::kEfficiency);

  bool should_exit = false;
  while (!should_exit) {
    std::unique_lock lock(reclaimables_mutex_);

    // Wait until there are reclaimable resource or if the manager should be
    // torn down.
    reclaimables_cv_.wait(
        lock, [&]() { return !reclaimables_.empty() || should_exit_; });

    // Don't reclaim resources when the lock is being held as this may gate
    // further reclaimables from being registered.
    Reclaimables resources_to_collect;
    std::swap(resources_to_collect, reclaimables_);

    // We can't read the ivar outside the lock. Read it here instead.
    should_exit = should_exit_;

    // We know what to collect. Unlock before doing anything else.
    lock.unlock();

    // Claim all resources while tracing.
    const uint64_t collected = resources_to_collect.size();
    {
      TRACE_EVENT0("Impeller", "ReclaimResources");
      resources_to_collect.clear();  // Redundant because of scope but here so
                                     // we can add a trace around it.
    }
    if (collected > 0u) {
      {
        std::scoped_lock destroyed_lock(reclaimables_mutex_);
        destroyed_count_ += collected;
      }
      destroyed_cv_.notify_all();
    }
  }
}

void ResourceManagerVK::Flush() {
  FML_DCHECK(waiter_.get_id() != std::this_thread::get_id())
      << "ResourceManagerVK::Flush would wait for its own thread.";
  TRACE_EVENT0("Impeller", "ResourceManagerVK::Flush");
  std::unique_lock lock(reclaimables_mutex_);
  const uint64_t target = reclaimed_count_;
  destroyed_cv_.wait(
      lock, [&]() { return destroyed_count_ >= target || should_exit_; });
}

void ResourceManagerVK::Reclaim(std::unique_ptr<ResourceVK> resource) {
  if (!resource) {
    return;
  }
  {
    std::scoped_lock lock(reclaimables_mutex_);
    reclaimables_.emplace_back(std::move(resource));
    reclaimed_count_++;
  }
  reclaimables_cv_.notify_one();
}

void ResourceManagerVK::Terminate() {
  // The thread should not be terminated more than once.
  FML_DCHECK(!should_exit_);

  {
    std::scoped_lock lock(reclaimables_mutex_);
    should_exit_ = true;
  }
  reclaimables_cv_.notify_one();
  destroyed_cv_.notify_all();
}

}  // namespace impeller
