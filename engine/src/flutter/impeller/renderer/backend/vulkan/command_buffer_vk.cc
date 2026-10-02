// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/vulkan/command_buffer_vk.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "fml/logging.h"
#include "impeller/renderer/backend/vulkan/blit_pass_vk.h"
#include "impeller/renderer/backend/vulkan/compute_pass_vk.h"
#include "impeller/renderer/backend/vulkan/context_vk.h"
#include "impeller/renderer/backend/vulkan/gpu_tracer_vk.h"
#include "impeller/renderer/backend/vulkan/render_pass_vk.h"
#include "impeller/renderer/backend/vulkan/texture_vk.h"
#include "impeller/renderer/command_buffer.h"
#include "impeller/renderer/render_target.h"

namespace impeller {

CommandBufferVK::CommandBufferVK(
    std::weak_ptr<const Context> context,
    std::shared_ptr<TrackedObjectsVK> tracked_objects)
    : CommandBuffer(std::move(context)),
      tracked_objects_(std::move(tracked_objects)) {}

CommandBufferVK::~CommandBufferVK() = default;

void CommandBufferVK::SetLabel(std::string_view label) const {
#ifdef IMPELLER_DEBUG
  auto context = context_.lock();
  if (!context) {
    return;
  }
  ContextVK::Cast(*context).SetDebugName(GetCommandBuffer(), label);
#endif  // IMPELLER_DEBUG
}

bool CommandBufferVK::IsValid() const {
  return true;
}

bool CommandBufferVK::OnSubmitCommands(bool block_on_schedule,
                                       CompletionCallback callback) {
  FML_UNREACHABLE()
}

void CommandBufferVK::OnWaitUntilCompleted() {}

void CommandBufferVK::OnWaitUntilScheduled() {}

std::shared_ptr<RenderPass> CommandBufferVK::OnCreateRenderPass(
    RenderTarget target) {
  auto context = context_.lock();
  if (!context) {
    return nullptr;
  }
  auto pass =
      std::shared_ptr<RenderPassVK>(new RenderPassVK(context,            //
                                                     target,             //
                                                     shared_from_this()  //
                                                     ));
  if (!pass->IsValid()) {
    return nullptr;
  }
  return pass;
}

std::shared_ptr<BlitPass> CommandBufferVK::OnCreateBlitPass() {
  if (!IsValid()) {
    return nullptr;
  }
  auto context = context_.lock();
  if (!context) {
    return nullptr;
  }
  auto pass = std::shared_ptr<BlitPassVK>(new BlitPassVK(
      shared_from_this(), ContextVK::Cast(*context).GetWorkarounds()));
  if (!pass->IsValid()) {
    return nullptr;
  }
  return pass;
}

std::shared_ptr<ComputePass> CommandBufferVK::OnCreateComputePass() {
  if (!IsValid()) {
    return nullptr;
  }
  auto context = context_.lock();
  if (!context) {
    return nullptr;
  }
  auto pass =
      std::shared_ptr<ComputePassVK>(new ComputePassVK(context,            //
                                                       shared_from_this()  //
                                                       ));
  if (!pass->IsValid()) {
    return nullptr;
  }
  return pass;
}

bool CommandBufferVK::EndCommandBuffer() const {
  if (external_images_finalized_) {
    return false;
  }
  ReleaseExternalImages();
  external_images_finalized_ = true;
  InsertDebugMarker("QueueSubmit");

  auto command_buffer = GetCommandBuffer();
  tracked_objects_->GetGPUProbe().RecordCmdBufferEnd(command_buffer);

  auto status = command_buffer.end();
  if (status != vk::Result::eSuccess) {
    VALIDATION_LOG << "Failed to end command buffer: " << vk::to_string(status);
    return false;
  }
  return true;
}

bool CommandBufferVK::HasPreparedExternalImage(
    const TextureSourceVK& texture) const {
  return std::any_of(external_images_.begin(), external_images_.end(),
                     [&texture](const ExternalImageUse& use) {
                       return use.source.get() == &texture;
                     });
}

bool CommandBufferVK::PrepareExternalImage(
    const std::shared_ptr<const TextureSourceVK>& texture) {
  if (!texture || external_images_finalized_ || !Track(texture)) {
    return false;
  }
  const auto ownership = texture->GetExternalImageOwnership();
  if (!ownership || HasPreparedExternalImage(*texture)) {
    return true;
  }
  auto context = context_.lock();
  if (!context) {
    return false;
  }
  const uint32_t local_family = static_cast<uint32_t>(
      ContextVK::Cast(*context).GetGraphicsQueue()->GetIndex().family);
  vk::ImageMemoryBarrier barrier;
  barrier.srcAccessMask = {};
  barrier.dstAccessMask =
      vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
  barrier.oldLayout = ownership->interchange_layout;
  barrier.newLayout = ownership->interchange_layout;
  barrier.srcQueueFamilyIndex = ownership->queue_family_index;
  barrier.dstQueueFamilyIndex = local_family;
  barrier.image = texture->GetImage();
  barrier.subresourceRange.aspectMask =
      ToImageAspectFlags(texture->GetTextureDescriptor().format);
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount =
      texture->GetTextureDescriptor().mip_count;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount =
      ToArrayLayerCount(texture->GetTextureDescriptor());

  external_images_.push_back(
      {texture, *ownership, local_family, ownership->interchange_layout});
  GetCommandBuffer().pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                     vk::PipelineStageFlagBits::eAllCommands,
                                     {}, nullptr, nullptr, barrier);
  texture->SetLayoutWithoutEncoding(ownership->interchange_layout);
  return true;
}

void CommandBufferVK::RecordExternalImageLayout(
    const TextureSourceVK& texture) {
  for (auto& use : external_images_) {
    if (use.source.get() == &texture) {
      use.final_layout = texture.GetLayout();
      return;
    }
  }
}

void CommandBufferVK::ReleaseExternalImages() const {
  for (const auto& use : external_images_) {
    // Restore the interchange layout locally before the ownership release.
    // Both sides of the transfer can consequently use the original external
    // GENERAL->GENERAL (or declared interchange) contract, even after tiles
    // sampled the image or copied into it in transfer-optimal layouts.
    if (use.final_layout != use.ownership.interchange_layout) {
      vk::ImageMemoryBarrier layout;
      layout.oldLayout = use.final_layout;
      layout.newLayout = use.ownership.interchange_layout;
      layout.srcAccessMask =
          vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
      layout.dstAccessMask =
          vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
      layout.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      layout.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      layout.image = use.source->GetImage();
      layout.subresourceRange.aspectMask =
          ToImageAspectFlags(use.source->GetTextureDescriptor().format);
      layout.subresourceRange.baseMipLevel = 0;
      layout.subresourceRange.levelCount =
          use.source->GetTextureDescriptor().mip_count;
      layout.subresourceRange.baseArrayLayer = 0;
      layout.subresourceRange.layerCount =
          ToArrayLayerCount(use.source->GetTextureDescriptor());
      GetCommandBuffer().pipelineBarrier(
          vk::PipelineStageFlagBits::eAllCommands,
          vk::PipelineStageFlagBits::eAllCommands, {}, nullptr, nullptr,
          layout);
    }
    use.source->SetLayoutWithoutEncoding(use.ownership.interchange_layout);
    vk::ImageMemoryBarrier barrier;
    barrier.srcAccessMask =
        vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
    barrier.dstAccessMask = {};
    barrier.oldLayout = use.ownership.interchange_layout;
    barrier.newLayout = use.ownership.interchange_layout;
    barrier.srcQueueFamilyIndex = use.local_queue_family;
    barrier.dstQueueFamilyIndex = use.ownership.queue_family_index;
    barrier.image = use.source->GetImage();
    barrier.subresourceRange.aspectMask =
        ToImageAspectFlags(use.source->GetTextureDescriptor().format);
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount =
        use.source->GetTextureDescriptor().mip_count;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount =
        ToArrayLayerCount(use.source->GetTextureDescriptor());
    GetCommandBuffer().pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                       vk::PipelineStageFlagBits::eBottomOfPipe,
                                       {}, nullptr, nullptr, barrier);
  }
}

vk::CommandBuffer CommandBufferVK::GetCommandBuffer() const {
  if (tracked_objects_) {
    return tracked_objects_->GetCommandBuffer();
  }
  return {};
}

bool CommandBufferVK::Track(const std::shared_ptr<SharedObjectVK>& object) {
  if (!IsValid()) {
    return false;
  }
  tracked_objects_->Track(object);
  return true;
}

bool CommandBufferVK::Track(const std::shared_ptr<const DeviceBuffer>& buffer) {
  if (!IsValid()) {
    return false;
  }
  tracked_objects_->Track(buffer);
  return true;
}

bool CommandBufferVK::Track(
    const std::shared_ptr<const TextureSourceVK>& texture) {
  if (!IsValid()) {
    return false;
  }
  tracked_objects_->Track(texture);
  return true;
}

bool CommandBufferVK::Track(const std::shared_ptr<const Texture>& texture) {
  if (!IsValid()) {
    return false;
  }
  if (!texture) {
    return true;
  }
  tracked_objects_->Track(texture);
  return Track(TextureVK::Cast(*texture).GetTextureSource());
}

fml::StatusOr<vk::DescriptorSet> CommandBufferVK::AllocateDescriptorSets(
    const vk::DescriptorSetLayout& layout,
    PipelineKey pipeline_key,
    const ContextVK& context) {
  if (!IsValid()) {
    return fml::Status(fml::StatusCode::kUnknown, "command encoder invalid");
  }

  return tracked_objects_->GetDescriptorPool().AllocateDescriptorSets(
      layout, pipeline_key, context);
}

void CommandBufferVK::PushDebugGroup(std::string_view label) const {
  if (!HasValidationLayers()) {
    return;
  }
  vk::DebugUtilsLabelEXT label_info;
  label_info.pLabelName = label.data();
  if (auto command_buffer = GetCommandBuffer()) {
    command_buffer.beginDebugUtilsLabelEXT(label_info);
  }
}

void CommandBufferVK::PopDebugGroup() const {
  if (!HasValidationLayers()) {
    return;
  }
  if (auto command_buffer = GetCommandBuffer()) {
    command_buffer.endDebugUtilsLabelEXT();
  }
}

void CommandBufferVK::InsertDebugMarker(std::string_view label) const {
  if (!HasValidationLayers()) {
    return;
  }
  vk::DebugUtilsLabelEXT label_info;
  label_info.pLabelName = label.data();
  if (auto command_buffer = GetCommandBuffer()) {
    command_buffer.insertDebugUtilsLabelEXT(label_info);
  }
}

DescriptorPoolVK& CommandBufferVK::GetDescriptorPool() const {
  return tracked_objects_->GetDescriptorPool();
}

}  // namespace impeller
