// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/vulkan/texture_source_vk.h"

#include <optional>

namespace impeller {

TextureSourceVK::TextureSourceVK(TextureDescriptor desc) : desc_(desc) {}

TextureSourceVK::~TextureSourceVK() = default;

void TextureSourceVK::ReleaseCachedFrameData() {
  frame_data_.clear();
  // A failed submission can return the exact producer wait to this source.
  // Release that native handle before a foreign collection callback may drop
  // its device owner; the OS render-completion FD has no such dependency.
  returned_acquire_semaphore_.reset();
}

const TextureDescriptor& TextureSourceVK::GetTextureDescriptor() const {
  return desc_;
}

std::shared_ptr<YUVConversionVK> TextureSourceVK::GetYUVConversion() const {
  return nullptr;
}

std::optional<WaitSemaphore> TextureSourceVK::ConsumeAcquireSemaphore() const {
  return std::nullopt;
}

std::optional<WaitSemaphore> TextureSourceVK::TakeAcquireSemaphoreForSubmit()
    const {
  if (returned_acquire_semaphore_) {
    WaitSemaphore wait;
    wait.semaphore = std::move(returned_acquire_semaphore_);
    wait.wait_stage = returned_acquire_stage_;
    return wait;
  }
  return ConsumeAcquireSemaphore();
}

void TextureSourceVK::ReturnAcquireSemaphoreFromFailedSubmit(
    WaitSemaphore wait) const {
  FML_DCHECK(!returned_acquire_semaphore_);
  returned_acquire_semaphore_ = std::move(wait.semaphore);
  returned_acquire_stage_ = wait.wait_stage;
}

std::shared_ptr<ExternalSemaphoreVK>
TextureSourceVK::CreateRenderCompleteSignalSemaphore(
    const std::shared_ptr<Context>& context) const {
  return nullptr;
}

void TextureSourceVK::SetRenderCompleteSyncFD(fml::UniqueFD sync_fd) const {}

fml::UniqueFD TextureSourceVK::TakeRenderCompleteSyncFD() const {
  return {};
}

std::optional<ExternalImageOwnershipVK>
TextureSourceVK::GetExternalImageOwnership() const {
  return std::nullopt;
}

size_t TextureSourceVK::GetAllocatedByteSize() const {
  return 0u;
}

void TextureSourceVK::RecordAvioImageUpload(bool raster_frame) const {}

vk::ImageLayout TextureSourceVK::GetLayout() const {
  return layout_;
}

vk::ImageLayout TextureSourceVK::SetLayoutWithoutEncoding(
    vk::ImageLayout layout) const {
  const auto old_layout = layout_;
  layout_ = layout;
  return old_layout;
}

fml::Status TextureSourceVK::SetLayout(const BarrierVK& barrier) const {
  const vk::ImageLayout old_layout =
      SetLayoutWithoutEncoding(barrier.new_layout);
  vk::ImageMemoryBarrier image_barrier;
  image_barrier.srcAccessMask = barrier.src_access;
  image_barrier.dstAccessMask = barrier.dst_access;
  image_barrier.oldLayout = old_layout;
  image_barrier.newLayout = barrier.new_layout;
  image_barrier.image = GetImage();
  image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  image_barrier.subresourceRange.aspectMask = ToImageAspectFlags(desc_.format);
  image_barrier.subresourceRange.baseMipLevel = barrier.base_mip_level;
  image_barrier.subresourceRange.levelCount =
      desc_.mip_count - barrier.base_mip_level;
  image_barrier.subresourceRange.baseArrayLayer = 0u;
  image_barrier.subresourceRange.layerCount = ToArrayLayerCount(desc_);

  barrier.cmd_buffer.pipelineBarrier(barrier.src_stage,  // src stage
                                     barrier.dst_stage,  // dst stage
                                     {},                 // dependency flags
                                     nullptr,            // memory barriers
                                     nullptr,            // buffer barriers
                                     image_barrier       // image barriers
  );

  return {};
}

bool RenderPassPolicyVK::operator==(const RenderPassPolicyVK& other) const {
  if (count != other.count) {
    return false;
  }
  for (size_t i = 0u; i < count && i < kCapacity; i++) {
    const vk::AttachmentDescription& a = attachments[i];
    const vk::AttachmentDescription& b = other.attachments[i];
    if (a.flags != b.flags || a.format != b.format || a.samples != b.samples ||
        a.loadOp != b.loadOp || a.storeOp != b.storeOp ||
        a.stencilLoadOp != b.stencilLoadOp ||
        a.stencilStoreOp != b.stencilStoreOp ||
        a.initialLayout != b.initialLayout || a.finalLayout != b.finalLayout) {
      return false;
    }
  }
  return true;
}

void TextureSourceVK::SetCachedFrameData(const FramebufferAndRenderPass& data,
                                         SampleCount sample_count,
                                         uint32_t mip_level,
                                         uint32_t slice,
                                         const RenderPassPolicyVK& policy) {
  for (auto& entry : frame_data_) {
    if (entry.sample_count == sample_count && entry.mip_level == mip_level &&
        entry.slice == slice && entry.policy == policy) {
      entry.data = data;
      return;
    }
  }
  frame_data_.push_back({sample_count, mip_level, slice, policy, data});
}

FramebufferAndRenderPass TextureSourceVK::GetCachedFrameData(
    SampleCount sample_count,
    uint32_t mip_level,
    uint32_t slice,
    const RenderPassPolicyVK* policy) const {
  for (const auto& entry : frame_data_) {
    if (entry.sample_count == sample_count && entry.mip_level == mip_level &&
        entry.slice == slice && (!policy || entry.policy == *policy)) {
      return entry.data;
    }
  }
  return {};
}

}  // namespace impeller
