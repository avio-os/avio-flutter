// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_TRACKED_OBJECTS_VK_H_
#define FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_TRACKED_OBJECTS_VK_H_

#include <memory>

#include "impeller/core/texture.h"
#include "impeller/renderer/backend/vulkan/context_vk.h"
#include "impeller/renderer/backend/vulkan/descriptor_pool_vk.h"
#include "impeller/renderer/backend/vulkan/gpu_tracer_vk.h"
#include "impeller/renderer/backend/vulkan/texture_source_vk.h"
#include "impeller/renderer/bounded_resource_owners.h"

namespace impeller {

/// @brief A per-frame object used to track resource lifetimes and allocate
///        command buffers and descriptor sets.
class TrackedObjectsVK {
 public:
  struct PendingSignalSemaphoreVK {
    std::shared_ptr<const TextureSourceVK> texture;
    std::shared_ptr<ExternalSemaphoreVK> semaphore;
  };

  explicit TrackedObjectsVK(const std::weak_ptr<const ContextVK>& context,
                            const std::shared_ptr<CommandPoolVK>& pool,
                            std::shared_ptr<DescriptorPoolVK> descriptor_pool,
                            std::unique_ptr<GPUProbe> probe);

  ~TrackedObjectsVK();

  bool IsValid() const;

  void Track(const std::shared_ptr<SharedObjectVK>& object);

  void Track(const std::shared_ptr<const DeviceBuffer>& buffer);

  void Track(const std::shared_ptr<const Texture>& texture);

  void Track(const std::shared_ptr<const TextureSourceVK>& texture);

  bool TrackPipeline(std::shared_ptr<void> pipeline) {
    return tracked_pipelines_.Retain(std::move(pipeline));
  }
  void AdoptResourceOwners(BoundedResourceOwners owners) {
    retained_resources_ = std::move(owners);
  }

  vk::CommandBuffer GetCommandBuffer() const;

  DescriptorPoolVK& GetDescriptorPool();

  GPUProbe& GetGPUProbe() const;

  // Only the owning graphics queue's SubmitLocked callback may consume waits.
  std::vector<WaitSemaphore> TakeWaitSemaphores();

  std::vector<PendingSignalSemaphoreVK> CreateSignalSemaphores(
      const std::shared_ptr<Context>& context);

 private:
  BoundedResourceOwners tracked_pipelines_;
  BoundedResourceOwners retained_resources_;
  std::shared_ptr<DescriptorPoolVK> desc_pool_;
  // `shared_ptr` since command buffers have a link to the command pool.
  std::shared_ptr<CommandPoolVK> pool_;
  vk::UniqueCommandBuffer buffer_;
  std::vector<std::shared_ptr<SharedObjectVK>> tracked_objects_;
  std::vector<std::shared_ptr<const DeviceBuffer>> tracked_buffers_;
  std::vector<std::shared_ptr<const Texture>> tracked_texture_wrappers_;
  std::vector<std::shared_ptr<const TextureSourceVK>> tracked_textures_;
  std::unique_ptr<GPUProbe> probe_;
  bool is_valid_ = false;

  TrackedObjectsVK(const TrackedObjectsVK&) = delete;

  TrackedObjectsVK& operator=(const TrackedObjectsVK&) = delete;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_TRACKED_OBJECTS_VK_H_
