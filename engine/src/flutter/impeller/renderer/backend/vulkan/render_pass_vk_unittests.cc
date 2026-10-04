// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>

#include "flutter/testing/testing.h"  // IWYU pragma: keep
#include "gtest/gtest.h"
#include "impeller/base/validation.h"
#include "impeller/core/formats.h"
#include "impeller/renderer/backend/vulkan/command_buffer_vk.h"
#include "impeller/renderer/backend/vulkan/render_pass_builder_vk.h"
#include "impeller/renderer/backend/vulkan/render_pass_vk.h"
#include "impeller/renderer/backend/vulkan/test/mock_vulkan.h"
#include "impeller/renderer/backend/vulkan/texture_vk.h"
#include "impeller/renderer/render_target.h"
#include "vulkan/vulkan_enums.hpp"

namespace impeller {
namespace testing {

class ExternalRenderTargetSourceVK final : public TextureSourceVK {
 public:
  ExternalRenderTargetSourceVK(vk::Image image,
                               vk::ImageView image_view,
                               TextureDescriptor desc)
      : TextureSourceVK(desc), image_(image), image_view_(image_view) {}

  vk::Image GetImage() const override { return image_; }
  vk::ImageView GetImageView() const override { return image_view_; }
  vk::ImageView GetRenderTargetView(uint32_t, uint32_t) const override {
    return image_view_;
  }
  bool IsSwapchainImage() const override { return true; }
  std::optional<ExternalImageOwnershipVK> GetExternalImageOwnership()
      const override {
    return ExternalImageOwnershipVK{};
  }

 private:
  vk::Image image_;
  vk::ImageView image_view_;
};

TEST(RenderPassVK, BoundedMSAARestrictsClearResolveAndEveryScissor) {
  auto context = MockVulkanContextBuilder().Build();
  RenderTargetAllocator allocator(context->GetResourceAllocator());
  auto target = allocator.CreateOffscreenMSAA(*context, {100, 100}, 1);
  auto resolve = target.GetColorAttachment(0).resolve_texture;
  ASSERT_TRUE(resolve);
  TextureVK::Cast(*resolve).SetLayoutWithoutEncoding(vk::ImageLayout::eGeneral);
  EXPECT_FALSE(target.SetRenderArea(IRect::MakeXYWH(-1, 0, 5, 5)));
  EXPECT_FALSE(target.SetRenderArea(IRect::MakeXYWH(0, 0, 0, 5)));
  EXPECT_FALSE(target.SetRenderArea(IRect::MakeXYWH(95, 95, 10, 10)));
  ASSERT_TRUE(target.SetRenderArea(IRect::MakeXYWH(20, 30, 40, 50)));
  auto buffer = context->CreateCommandBuffer();
  auto pass = buffer->CreateRenderPass(target);
  ASSERT_TRUE(pass);
  auto raw = CommandBufferVK::Cast(*buffer).GetCommandBuffer();
  const auto& areas = GetRecordedRenderAreas(raw);
  ASSERT_EQ(areas.size(), 1u);
  EXPECT_EQ(areas[0].offset.x, 20);
  EXPECT_EQ(areas[0].offset.y, 30);
  EXPECT_EQ(areas[0].extent.width, 40u);
  EXPECT_EQ(areas[0].extent.height, 50u);
  pass->SetScissor(IRect32::MakeSize(ISize(100, 100)));
  pass->SetScissor(IRect32::MakeXYWH(40, 50, 50, 50));
  const auto& scissors = GetRecordedScissors(raw);
  ASSERT_EQ(scissors.size(), 3u);
  EXPECT_EQ(scissors[0].offset.x, 20);
  EXPECT_EQ(scissors[1].extent.width, 40u);
  EXPECT_EQ(scissors[2].offset.x, 40);
  EXPECT_EQ(scissors[2].offset.y, 50);
  EXPECT_EQ(scissors[2].extent.width, 20u);
  EXPECT_EQ(scissors[2].extent.height, 30u);
  EXPECT_EQ(target.GetSampleCount(), SampleCount::kCount4);
  EXPECT_EQ(target.GetRenderTargetSize(), ISize(100, 100));
  EXPECT_TRUE(pass->EncodeCommands());
}

TEST(RenderPassVK, BoundedPassRejectsUnknownContentsWithoutWidening) {
  ScopedValidationDisable validation;
  auto context = MockVulkanContextBuilder().Build();
  RenderTargetAllocator allocator(context->GetResourceAllocator());
  auto target = allocator.CreateOffscreenMSAA(*context, {100, 100}, 1);
  ASSERT_TRUE(target.SetRenderArea(IRect::MakeXYWH(20, 30, 40, 50)));
  auto buffer = context->CreateCommandBuffer();
  EXPECT_FALSE(buffer->CreateRenderPass(target));
  EXPECT_TRUE(
      GetRecordedRenderAreas(CommandBufferVK::Cast(*buffer).GetCommandBuffer())
          .empty());
  EXPECT_TRUE(GetMockVulkanQueueSubmitBatchCounts().empty());
}

TEST(RenderPassVK, ExplicitFullAreaInitializesNewLayerWithoutWidening) {
  auto context = MockVulkanContextBuilder().Build();
  RenderTargetAllocator allocator(context->GetResourceAllocator());
  auto target = allocator.CreateOffscreen(*context, {100, 100}, 1);
  ASSERT_TRUE(target.SetRenderArea(IRect::MakeSize(ISize(100, 100))));
  ASSERT_TRUE(target.SetContentRect(IRect::MakeSize(ISize(40, 30))));
  auto buffer = context->CreateCommandBuffer();
  auto pass = buffer->CreateRenderPass(target);
  ASSERT_TRUE(pass);
  const auto& areas =
      GetRecordedRenderAreas(CommandBufferVK::Cast(*buffer).GetCommandBuffer());
  ASSERT_EQ(areas.size(), 1u);
  EXPECT_EQ(areas[0].offset.x, 0);
  EXPECT_EQ(areas[0].offset.y, 0);
  EXPECT_EQ(areas[0].extent.width, 100u);
  EXPECT_EQ(areas[0].extent.height, 100u);
  EXPECT_EQ(target.GetContentRect(), IRect::MakeSize(ISize(40, 30)));
  EXPECT_TRUE(pass->EncodeCommands());
}

TEST(RenderPassVK, BoundedAndFullPassesDoNotShareIncompatibleCachedPolicy) {
  auto context = MockVulkanContextBuilder().Build();
  RenderTargetAllocator allocator(context->GetResourceAllocator());
  auto full = allocator.CreateOffscreenMSAA(*context, {100, 100}, 1);
  auto& resolve = TextureVK::Cast(*full.GetColorAttachment(0).resolve_texture);
  auto run = [&](const RenderTarget& target, IRect expected) {
    auto buffer = context->CreateCommandBuffer();
    auto pass = buffer->CreateRenderPass(target);
    ASSERT_TRUE(pass);
    const auto& areas = GetRecordedRenderAreas(
        CommandBufferVK::Cast(*buffer).GetCommandBuffer());
    ASSERT_EQ(areas.size(), 1u);
    EXPECT_EQ(areas[0].offset.x, expected.GetX());
    EXPECT_EQ(areas[0].offset.y, expected.GetY());
    EXPECT_EQ(areas[0].extent.width, expected.GetWidth());
    EXPECT_EQ(areas[0].extent.height, expected.GetHeight());
    EXPECT_TRUE(pass->EncodeCommands());
  };
  run(full, IRect::MakeSize(ISize(100, 100)));
  auto cached = resolve.GetCachedFrameData(SampleCount::kCount4, 0, 0);
  ASSERT_TRUE(cached.render_pass);
  auto bounded = full;
  ASSERT_TRUE(bounded.SetRenderArea(IRect::MakeXYWH(20, 30, 40, 50)));
  run(bounded, *bounded.GetRenderArea());
  EXPECT_EQ(resolve.GetCachedFrameData(SampleCount::kCount4, 0, 0).render_pass,
            cached.render_pass);
  run(full, IRect::MakeSize(ISize(100, 100)));
}

TEST(RenderPassVK, PolicyDistinguishesLoadActionAndInitialLayout) {
  auto policy = [](LoadAction load, vk::ImageLayout current) {
    RenderPassBuilderVK builder;
    builder.SetColorAttachment(0, PixelFormat::kR8G8B8A8UNormInt,
                               SampleCount::kCount1, load, StoreAction::kStore,
                               current);
    return builder.GetPolicy();
  };
  const auto clear = policy(LoadAction::kClear, vk::ImageLayout::eGeneral);
  ASSERT_EQ(clear.count, 1u);
  EXPECT_EQ(clear.attachments[0].loadOp, vk::AttachmentLoadOp::eClear);
  EXPECT_EQ(clear.attachments[0].initialLayout, vk::ImageLayout::eUndefined);
  // A clear discards contents whatever the current layout is.
  EXPECT_TRUE(clear ==
              policy(LoadAction::kClear,
                     vk::ImageLayout::eShaderReadOnlyOptimal));
  const auto load = policy(LoadAction::kLoad, vk::ImageLayout::eGeneral);
  EXPECT_FALSE(clear == load);
  EXPECT_EQ(load.attachments[0].loadOp, vk::AttachmentLoadOp::eLoad);
  EXPECT_EQ(load.attachments[0].initialLayout, vk::ImageLayout::eGeneral);
  EXPECT_FALSE(load == policy(LoadAction::kLoad,
                              vk::ImageLayout::eShaderReadOnlyOptimal));
}

// The Coverage root is an imported single-sample image without a render area.
// Its first segment clears it; every later segment, tile composite and clip
// parent pass loads the composited prefix in a new command buffer. A cached
// render pass must never replay the first segment's CLEAR over that prefix.
TEST(RenderPassVK, LaterSegmentLoadsTheParentAfterAnEarlierClear) {
  auto context = MockVulkanContextBuilder().Build();
  TextureDescriptor desc;
  desc.size = ISize(32, 32);
  desc.format = PixelFormat::kR8G8B8A8UNormInt;
  desc.storage_mode = StorageMode::kDevicePrivate;
  desc.usage = TextureUsage::kRenderTarget | TextureUsage::kShaderRead;
  auto allocation = context->GetResourceAllocator()->CreateTexture(desc);
  ASSERT_TRUE(allocation);
  const auto& image = TextureVK::Cast(*allocation);
  auto source = std::make_shared<ExternalRenderTargetSourceVK>(
      image.GetImage(), image.GetImageView(), desc);
  auto parent = std::make_shared<TextureVK>(context, source);
  auto run_segment = [&](LoadAction load) {
    RenderTarget target;
    ColorAttachment colour;
    colour.texture = parent;
    colour.load_action = load;
    colour.store_action = StoreAction::kStore;
    colour.clear_color = Color::BlackTransparent();
    target.SetColorAttachment(colour, 0);
    auto buffer = context->CreateCommandBuffer();
    auto pass = buffer->CreateRenderPass(target);
    ASSERT_TRUE(pass);
    ASSERT_TRUE(pass->EncodeCommands());
    ASSERT_TRUE(CommandBufferVK::Cast(*buffer).EndCommandBuffer());
  };
  auto creates = [&] {
    const auto called = GetMockVulkanFunctions(context->GetDevice());
    return std::count(called->begin(), called->end(), "vkCreateRenderPass");
  };

  run_segment(LoadAction::kClear);
  ASSERT_EQ(GetLastRenderPassAttachments().size(), 1u);
  EXPECT_EQ(GetLastRenderPassAttachments()[0].loadOp,
            VK_ATTACHMENT_LOAD_OP_CLEAR);
  EXPECT_EQ(GetLastRenderPassAttachments()[0].initialLayout,
            VK_IMAGE_LAYOUT_UNDEFINED);
  const auto after_clear = creates();

  run_segment(LoadAction::kLoad);
  EXPECT_EQ(creates(), after_clear + 1);
  ASSERT_EQ(GetLastRenderPassAttachments().size(), 1u);
  EXPECT_EQ(GetLastRenderPassAttachments()[0].loadOp,
            VK_ATTACHMENT_LOAD_OP_LOAD);
  EXPECT_EQ(GetLastRenderPassAttachments()[0].initialLayout,
            VK_IMAGE_LAYOUT_GENERAL);

  // Identical policies still share their cached render pass and framebuffer.
  const auto both_policies = creates();
  run_segment(LoadAction::kLoad);
  run_segment(LoadAction::kClear);
  EXPECT_EQ(creates(), both_policies);
}

TEST(RenderPassVK, DoesNotRedundantlySetStencil) {
  std::shared_ptr<ContextVK> context = MockVulkanContextBuilder().Build();
  std::shared_ptr<Context> copy = context;
  auto cmd_buffer = context->CreateCommandBuffer();

  RenderTargetAllocator allocator(context->GetResourceAllocator());
  RenderTarget target = allocator.CreateOffscreenMSAA(*copy.get(), {1, 1}, 1);

  std::shared_ptr<RenderPass> render_pass =
      cmd_buffer->CreateRenderPass(target);

  // Stencil reference set once at buffer start.
  auto called_functions = GetMockVulkanFunctions(context->GetDevice());
  EXPECT_EQ(std::count(called_functions->begin(), called_functions->end(),
                       "vkCmdSetStencilReference"),
            1);

  // Duplicate stencil ref is not replaced.
  render_pass->SetStencilReference(0);
  render_pass->SetStencilReference(0);
  render_pass->SetStencilReference(0);

  called_functions = GetMockVulkanFunctions(context->GetDevice());
  EXPECT_EQ(std::count(called_functions->begin(), called_functions->end(),
                       "vkCmdSetStencilReference"),
            1);

  // Different stencil value is updated.
  render_pass->SetStencilReference(1);
  called_functions = GetMockVulkanFunctions(context->GetDevice());
  EXPECT_EQ(std::count(called_functions->begin(), called_functions->end(),
                       "vkCmdSetStencilReference"),
            2);
}

// Regression guard for the bug where `RenderPassVK::SetViewport` silently
// dropped the user's X and Y offsets and the depth range, only honoring
// width and height.
TEST(RenderPassVK, SetViewportPropagatesAllUserSuppliedFields) {
  std::shared_ptr<ContextVK> context = MockVulkanContextBuilder().Build();
  auto cmd_buffer = context->CreateCommandBuffer();

  RenderTargetAllocator allocator(context->GetResourceAllocator());
  RenderTarget target = allocator.CreateOffscreenMSAA(*context, {100, 100},
                                                      /*mip_count=*/1);

  std::shared_ptr<RenderPass> render_pass =
      cmd_buffer->CreateRenderPass(target);

  // The render pass constructor sets an initial full-target viewport. Capture
  // a reference to the recorded viewport list before the user-driven call so
  // we can isolate the call under test.
  VkCommandBuffer raw_cmd_buffer =
      CommandBufferVK::Cast(*cmd_buffer).GetCommandBuffer();
  const std::vector<VkViewport>& recorded =
      GetRecordedViewports(raw_cmd_buffer);
  ASSERT_EQ(recorded.size(), 1u);  // Initial full-target viewport.

  render_pass->SetViewport(Viewport{
      .rect = Rect::MakeXYWH(25, 10, 50, 80),
      .depth_range = DepthRange{.z_near = 0.25f, .z_far = 0.75f},
  });

  ASSERT_EQ(recorded.size(), 2u);
  const VkViewport& vp = recorded[1];
  EXPECT_FLOAT_EQ(vp.x, 25.0f);
  // Y is computed for the negative-height Y-flip: `y + height` of the
  // original rect.
  EXPECT_FLOAT_EQ(vp.y, 90.0f);
  EXPECT_FLOAT_EQ(vp.width, 50.0f);
  EXPECT_FLOAT_EQ(vp.height, -80.0f);
  EXPECT_FLOAT_EQ(vp.minDepth, 0.25f);
  EXPECT_FLOAT_EQ(vp.maxDepth, 0.75f);
}

TEST(RenderPassVK, TransfersExternalRenderTargetOwnership) {
  auto context = MockVulkanContextBuilder().Build();

  TextureDescriptor desc;
  desc.size = ISize(32, 32);
  desc.format = PixelFormat::kR8G8B8A8UNormInt;
  desc.storage_mode = StorageMode::kDevicePrivate;
  desc.sample_count = SampleCount::kCount1;
  desc.usage = TextureUsage::kRenderTarget;

  auto allocated = context->GetResourceAllocator()->CreateTexture(desc);
  ASSERT_TRUE(allocated);
  const auto& allocated_vk = TextureVK::Cast(*allocated);
  auto source = std::make_shared<ExternalRenderTargetSourceVK>(
      allocated_vk.GetImage(), allocated_vk.GetImageView(), desc);
  auto external_texture = std::make_shared<TextureVK>(context, source);

  RenderTarget target;
  ColorAttachment color;
  color.texture = external_texture;
  color.load_action = LoadAction::kLoad;
  color.store_action = StoreAction::kStore;
  target.SetColorAttachment(color, 0);

  auto command_buffer = context->CreateCommandBuffer();
  auto render_pass = command_buffer->CreateRenderPass(target);
  ASSERT_TRUE(render_pass);
  ASSERT_TRUE(render_pass->EncodeCommands());

  auto& barriers = GetImageMemoryBarriers(
      CommandBufferVK::Cast(*command_buffer).GetCommandBuffer());
  // Ending one pass does not hand the image back while the same command
  // buffer can still seed, replay, and copy later coverage tiles.
  ASSERT_EQ(barriers.size(), 1u);
  ASSERT_TRUE(CommandBufferVK::Cast(*command_buffer).EndCommandBuffer());
  ASSERT_EQ(barriers.size(), 2u);
  const uint32_t local_family =
      static_cast<uint32_t>(context->GetGraphicsQueue()->GetIndex().family);

  EXPECT_EQ(barriers[0].srcQueueFamilyIndex, VK_QUEUE_FAMILY_FOREIGN_EXT);
  EXPECT_EQ(barriers[0].dstQueueFamilyIndex, local_family);
  EXPECT_EQ(barriers[0].oldLayout, VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(barriers[0].newLayout, VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(barriers[0].subresourceRange.layerCount, 1u);
  EXPECT_EQ(
      barriers[0].dstAccessMask,
      VkAccessFlags{VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT});

  EXPECT_EQ(barriers[1].srcQueueFamilyIndex, local_family);
  EXPECT_EQ(barriers[1].dstQueueFamilyIndex, VK_QUEUE_FAMILY_FOREIGN_EXT);
  EXPECT_EQ(barriers[1].oldLayout, VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(barriers[1].newLayout, VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(barriers[1].subresourceRange.layerCount, 1u);
  EXPECT_EQ(barriers[1].dstAccessMask, VkAccessFlags{0});
}

TEST(RenderPassVK, ExternalParentRemainsOwnedAcrossSeedAndTileCopies) {
  auto context = MockVulkanContextBuilder().Build();
  TextureDescriptor desc;
  desc.size = ISize(32, 32);
  desc.format = PixelFormat::kR8G8B8A8UNormInt;
  desc.storage_mode = StorageMode::kDevicePrivate;
  desc.usage = TextureUsage::kRenderTarget | TextureUsage::kShaderRead;
  auto allocation = context->GetResourceAllocator()->CreateTexture(desc);
  ASSERT_TRUE(allocation);
  const auto& image = TextureVK::Cast(*allocation);
  auto source = std::make_shared<ExternalRenderTargetSourceVK>(
      image.GetImage(), image.GetImageView(), desc);
  auto parent = std::make_shared<TextureVK>(context, source);
  RenderTarget target;
  ColorAttachment colour;
  colour.texture = parent;
  colour.load_action = LoadAction::kClear;
  colour.store_action = StoreAction::kStore;
  target.SetColorAttachment(colour, 0);
  auto buffer = context->CreateCommandBuffer();
  auto& buffer_vk = CommandBufferVK::Cast(*buffer);
  auto initialize = buffer->CreateRenderPass(target);
  ASSERT_TRUE(initialize);
  ASSERT_TRUE(initialize->EncodeCommands());
  ASSERT_TRUE(buffer_vk.HasPreparedExternalImage(*source));
  for (int tile = 0; tile < 2; tile++) {
    auto blit = buffer->CreateBlitPass();
    ASSERT_TRUE(blit);
    ASSERT_TRUE(blit->ConvertTextureToShaderRead(parent));
    EXPECT_EQ(source->GetLayout(), vk::ImageLayout::eShaderReadOnlyOptimal);
    // A freshly resolved tile is copied back after the parent shader read.
    auto resolved = context->GetResourceAllocator()->CreateTexture(desc);
    ASSERT_TRUE(resolved);
    TextureVK::Cast(*resolved).SetLayoutWithoutEncoding(
        vk::ImageLayout::eShaderReadOnlyOptimal);
    ASSERT_TRUE(blit->AddCopy(resolved, parent, IRect::MakeXYWH(0, 0, 8, 8),
                              IPoint{tile * 8, 0}));
    ASSERT_TRUE(blit->EncodeCommands());
  }
  const auto raw = buffer_vk.GetCommandBuffer();
  const auto& recorded = GetRecordedImageBarriers(raw);
  uint32_t acquires = 0;
  uint32_t releases = 0;
  uint32_t shader_reads_before_transfer = 0;
  for (const auto& entry : recorded) {
    const auto& barrier = entry.barrier;
    if (barrier.image != static_cast<VkImage>(source->GetImage())) {
      continue;
    }
    acquires += barrier.srcQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT;
    releases += barrier.dstQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT;
    if (barrier.oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
        barrier.newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
      shader_reads_before_transfer++;
      EXPECT_NE(entry.source_stage & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u);
      EXPECT_NE(barrier.srcAccessMask & VK_ACCESS_MEMORY_READ_BIT, 0u);
      EXPECT_EQ(entry.destination_stage, VK_PIPELINE_STAGE_TRANSFER_BIT);
      EXPECT_EQ(barrier.dstAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT);
    }
  }
  EXPECT_EQ(acquires, 1u);
  EXPECT_EQ(releases, 0u);
  EXPECT_EQ(shader_reads_before_transfer, 2u);
  ASSERT_TRUE(buffer_vk.EndCommandBuffer());
  ASSERT_GE(recorded.size(), 2u);
  const auto& transition = recorded[recorded.size() - 2].barrier;
  EXPECT_EQ(transition.oldLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  EXPECT_EQ(transition.newLayout, VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(transition.srcQueueFamilyIndex, VK_QUEUE_FAMILY_IGNORED);
  const auto& release = recorded.back();
  EXPECT_EQ(release.barrier.oldLayout, VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(release.barrier.newLayout, VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(release.barrier.dstQueueFamilyIndex, VK_QUEUE_FAMILY_FOREIGN_EXT);
  EXPECT_NE(release.source_stage & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u);
  EXPECT_EQ(release.destination_stage, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  EXPECT_EQ(release.barrier.dstAccessMask, 0u);
  EXPECT_EQ(source->GetLayout(), vk::ImageLayout::eGeneral);
}

TEST(RenderPassVK, DeferredReleaseUsesItsOwnCommandBufferLayout) {
  auto context = MockVulkanContextBuilder().Build();
  TextureDescriptor desc;
  desc.size = ISize(8, 8);
  desc.format = PixelFormat::kR8G8B8A8UNormInt;
  desc.usage = TextureUsage::kRenderTarget | TextureUsage::kShaderRead;
  auto allocation = context->GetResourceAllocator()->CreateTexture(desc);
  ASSERT_TRUE(allocation);
  const auto& image = TextureVK::Cast(*allocation);
  auto source = std::make_shared<ExternalRenderTargetSourceVK>(
      image.GetImage(), image.GetImageView(), desc);
  auto external = std::make_shared<TextureVK>(context, source);
  auto first = context->CreateCommandBuffer();
  auto second = context->CreateCommandBuffer();
  auto first_blit = first->CreateBlitPass();
  auto second_blit = second->CreateBlitPass();
  ASSERT_TRUE(first_blit);
  ASSERT_TRUE(second_blit);
  ASSERT_TRUE(first_blit->ConvertTextureToShaderRead(external));
  auto input = context->GetResourceAllocator()->CreateTexture(desc);
  ASSERT_TRUE(input);
  ASSERT_TRUE(second_blit->AddCopy(input, external));
  EXPECT_EQ(source->GetLayout(), vk::ImageLayout::eTransferDstOptimal);
  auto& first_vk = CommandBufferVK::Cast(*first);
  auto& second_vk = CommandBufferVK::Cast(*second);
  ASSERT_TRUE(first_vk.EndCommandBuffer());
  const auto& first_barriers =
      GetImageMemoryBarriers(first_vk.GetCommandBuffer());
  ASSERT_GE(first_barriers.size(), 2u);
  EXPECT_EQ(first_barriers[first_barriers.size() - 2].oldLayout,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  ASSERT_TRUE(second_vk.EndCommandBuffer());
  const auto& second_barriers =
      GetImageMemoryBarriers(second_vk.GetCommandBuffer());
  ASSERT_GE(second_barriers.size(), 2u);
  EXPECT_EQ(second_barriers[second_barriers.size() - 2].oldLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
}

}  // namespace testing
}  // namespace impeller
