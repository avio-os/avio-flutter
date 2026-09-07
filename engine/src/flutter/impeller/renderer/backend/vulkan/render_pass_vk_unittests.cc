// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

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
  ASSERT_EQ(barriers.size(), 2u);
  const uint32_t local_family =
      static_cast<uint32_t>(context->GetGraphicsQueue()->GetIndex().family);

  EXPECT_EQ(barriers[0].srcQueueFamilyIndex, VK_QUEUE_FAMILY_FOREIGN_EXT);
  EXPECT_EQ(barriers[0].dstQueueFamilyIndex, local_family);
  EXPECT_EQ(barriers[0].oldLayout, VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(barriers[0].newLayout, VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(barriers[0].subresourceRange.layerCount, 1u);
  EXPECT_EQ(barriers[0].dstAccessMask,
            VkAccessFlags{VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT});

  EXPECT_EQ(barriers[1].srcQueueFamilyIndex, local_family);
  EXPECT_EQ(barriers[1].dstQueueFamilyIndex, VK_QUEUE_FAMILY_FOREIGN_EXT);
  EXPECT_EQ(barriers[1].oldLayout, VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(barriers[1].newLayout, VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(barriers[1].subresourceRange.layerCount, 1u);
  EXPECT_EQ(barriers[1].dstAccessMask, VkAccessFlags{0});
}

}  // namespace testing
}  // namespace impeller
