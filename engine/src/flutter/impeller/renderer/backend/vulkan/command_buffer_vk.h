// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_COMMAND_BUFFER_VK_H_
#define FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_COMMAND_BUFFER_VK_H_

#include "fml/status_or.h"
#include "impeller/base/backend_cast.h"
#include "impeller/renderer/backend/vulkan/command_queue_vk.h"
#include "impeller/renderer/backend/vulkan/descriptor_pool_vk.h"
#include "impeller/renderer/backend/vulkan/texture_source_vk.h"
#include "impeller/renderer/backend/vulkan/tracked_objects_vk.h"
#include "impeller/renderer/backend/vulkan/vk.h"
#include "impeller/renderer/command_buffer.h"

namespace impeller {

class ContextVK;
class CommandEncoderFactoryVK;
class CommandEncoderVK;

class CommandBufferVK final
    : public CommandBuffer,
      public BackendCast<CommandBufferVK, CommandBuffer>,
      public std::enable_shared_from_this<CommandBufferVK> {
 public:
  // |CommandBuffer|
  ~CommandBufferVK() override;

  // Encoder Functionality

  /// @brief Ensure that [object] is kept alive until this command buffer
  ///        completes execution.
  bool Track(const std::shared_ptr<SharedObjectVK>& object);

  /// @brief Ensure that [buffer] is kept alive until this command buffer
  ///        completes execution.
  bool Track(const std::shared_ptr<const DeviceBuffer>& buffer);

  /// @brief Ensure that [texture] is kept alive until this command buffer
  ///       completes execution.
  bool Track(const std::shared_ptr<const Texture>& texture);

  /// @brief Ensure that [texture] is kept alive until this command buffer
  ///        completes execution.
  bool Track(const std::shared_ptr<const TextureSourceVK>& texture);

  /// Acquire a declared external image before beginning a render pass or blit.
  /// Ownership remains local for all uses in this command buffer and is
  /// released at EndCommandBuffer, before its completion semaphore is signaled.
  /// This must not be called while a Vulkan render pass is active.
  bool PrepareExternalImage(
      const std::shared_ptr<const TextureSourceVK>& texture);

  /// Sampling cannot acquire an image inside an already active render pass.
  /// An external source must have been explicitly prepared beforehand.
  bool HasPreparedExternalImage(const TextureSourceVK& texture) const;

  /// Remember this command buffer's own last image layout. A later recorded
  /// command buffer can update the source's shared CPU bookkeeping before this
  /// one is ended, so final release cannot infer its old layout from the
  /// source.
  void RecordExternalImageLayout(const TextureSourceVK& texture);

  /// @brief Retrieve the native command buffer from this object.
  vk::CommandBuffer GetCommandBuffer() const;

  /// @brief Push a debug group.
  ///
  /// This label is only visible in debuggers like RenderDoc. This function is
  /// ignored in release builds.
  void PushDebugGroup(std::string_view label) const;

  /// @brief Pop the previous debug group.
  ///
  /// This label is only visible in debuggers like RenderDoc. This function is
  /// ignored in release builds.
  void PopDebugGroup() const;

  /// @brief Insert a new debug marker.
  ///
  /// This label is only visible in debuggers like RenderDoc. This function is
  /// ignored in release builds.
  void InsertDebugMarker(std::string_view label) const;

  /// @brief End recording of the current command buffer.
  bool EndCommandBuffer() const;

  /// @brief Allocate a new descriptor set for the given [layout].
  fml::StatusOr<vk::DescriptorSet> AllocateDescriptorSets(
      const vk::DescriptorSetLayout& layout,
      PipelineKey pipeline_key,
      const ContextVK& context);

  // Visible for testing.
  DescriptorPoolVK& GetDescriptorPool() const;

 private:
  friend class ContextVK;
  friend class CommandQueueVK;

  std::shared_ptr<TrackedObjectsVK> tracked_objects_;

  struct ExternalImageUse {
    std::shared_ptr<const TextureSourceVK> source;
    ExternalImageOwnershipVK ownership;
    uint32_t local_queue_family;
    vk::ImageLayout final_layout;
  };
  mutable std::vector<ExternalImageUse> external_images_;
  mutable bool external_images_finalized_ = false;

  void ReleaseExternalImages() const;

  CommandBufferVK(std::weak_ptr<const Context> context,
                  std::shared_ptr<TrackedObjectsVK> tracked_objects);

  // |CommandBuffer|
  void SetLabel(std::string_view label) const override;

  // |CommandBuffer|
  bool IsValid() const override;

  // |CommandBuffer|
  bool OnSubmitCommands(bool block_on_schedule,
                        CompletionCallback callback) override;

  // |CommandBuffer|
  void OnWaitUntilCompleted() override;

  // |CommandBuffer|
  void OnWaitUntilScheduled() override;

  // |CommandBuffer|
  std::shared_ptr<RenderPass> OnCreateRenderPass(RenderTarget target) override;

  // |CommandBuffer|
  std::shared_ptr<BlitPass> OnCreateBlitPass() override;

  // |CommandBuffer|
  std::shared_ptr<ComputePass> OnCreateComputePass() override;

  CommandBufferVK(const CommandBufferVK&) = delete;

  CommandBufferVK& operator=(const CommandBufferVK&) = delete;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_COMMAND_BUFFER_VK_H_
