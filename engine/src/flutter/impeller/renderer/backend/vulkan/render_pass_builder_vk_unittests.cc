// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/testing/testing.h"  // IWYU pragma: keep
#include "gtest/gtest.h"
#include "impeller/core/formats.h"
#include "impeller/renderer/backend/vulkan/render_pass_builder_vk.h"
#include "impeller/renderer/backend/vulkan/test/mock_vulkan.h"
#include "vulkan/vulkan_enums.hpp"

namespace impeller {
namespace testing {

TEST(RenderPassBuilder, BoundedResolvePreservesOutsideWithoutLoadingMSAA) {
  RenderPassBuilderVK builder;
  builder.SetColorAttachment(
      0, PixelFormat::kR8G8B8A8UNormInt, SampleCount::kCount4,
      LoadAction::kClear, StoreAction::kMultisampleResolve,
      vk::ImageLayout::eGeneral, true, vk::ImageLayout::eGeneral);
  ASSERT_TRUE(builder.GetColor0());
  ASSERT_TRUE(builder.GetColor0Resolve());
  EXPECT_EQ(builder.GetColor0()->initialLayout, vk::ImageLayout::eUndefined);
  EXPECT_EQ(builder.GetColor0()->loadOp, vk::AttachmentLoadOp::eClear);
  EXPECT_EQ(builder.GetColor0Resolve()->initialLayout,
            vk::ImageLayout::eGeneral);
  EXPECT_EQ(builder.GetColor0Resolve()->loadOp,
            vk::AttachmentLoadOp::eDontCare);
  EXPECT_EQ(builder.GetColor0Resolve()->storeOp, vk::AttachmentStoreOp::eStore);
}

TEST(RenderPassBuilder, CreatesRenderPassWithNoDepthStencil) {
  RenderPassBuilderVK builder = RenderPassBuilderVK();
  auto const context = MockVulkanContextBuilder().Build();

  // Create a single color attachment with a transient depth stencil.
  builder.SetColorAttachment(0, PixelFormat::kR8G8B8A8UNormInt,
                             SampleCount::kCount1, LoadAction::kClear,
                             StoreAction::kStore);

  auto render_pass = builder.Build(context->GetDevice());

  EXPECT_TRUE(!!render_pass);
  EXPECT_FALSE(builder.GetDepthStencil().has_value());
  const auto& dependencies = GetLastRenderPassDependencies();
  ASSERT_EQ(dependencies.size(), 3u);
  EXPECT_EQ(dependencies[0].dstAccessMask,
            VkAccessFlags{VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT});
}

TEST(RenderPassBuilder, RenderPassWithLoadOpUsesCurrentLayout) {
  RenderPassBuilderVK builder = RenderPassBuilderVK();
  auto const context = MockVulkanContextBuilder().Build();

  builder.SetColorAttachment(0, PixelFormat::kR8G8B8A8UNormInt,
                             SampleCount::kCount1, LoadAction::kLoad,
                             StoreAction::kStore,
                             vk::ImageLayout::eColorAttachmentOptimal);

  auto render_pass = builder.Build(context->GetDevice());

  EXPECT_TRUE(!!render_pass);

  std::optional<vk::AttachmentDescription> maybe_color = builder.GetColor0();
  ASSERT_TRUE(maybe_color.has_value());
  if (!maybe_color.has_value()) {
    return;
  }
  vk::AttachmentDescription color = maybe_color.value();

  EXPECT_EQ(color.initialLayout, vk::ImageLayout::eColorAttachmentOptimal);
  EXPECT_EQ(color.finalLayout, vk::ImageLayout::eShaderReadOnlyOptimal);
  EXPECT_EQ(color.loadOp, vk::AttachmentLoadOp::eLoad);
  EXPECT_EQ(color.storeOp, vk::AttachmentStoreOp::eStore);
  const auto& dependencies = GetLastRenderPassDependencies();
  ASSERT_EQ(dependencies.size(), 3u);
  EXPECT_EQ(dependencies[0].dstAccessMask,
            VkAccessFlags{VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT});
}

TEST(RenderPassBuilder, CreatesRenderPassWithCombinedDepthStencil) {
  RenderPassBuilderVK builder = RenderPassBuilderVK();
  auto const context = MockVulkanContextBuilder().Build();

  // Create a single color attachment with a transient depth stencil.
  builder.SetColorAttachment(0, PixelFormat::kR8G8B8A8UNormInt,
                             SampleCount::kCount1, LoadAction::kClear,
                             StoreAction::kStore, vk::ImageLayout::eGeneral);
  builder.SetDepthStencilAttachment(PixelFormat::kD24UnormS8Uint,
                                    SampleCount::kCount1, LoadAction::kDontCare,
                                    StoreAction::kDontCare);

  auto render_pass = builder.Build(context->GetDevice());

  EXPECT_TRUE(!!render_pass);

  std::optional<vk::AttachmentDescription> maybe_color = builder.GetColor0();
  ASSERT_TRUE(maybe_color.has_value());
  if (!maybe_color.has_value()) {
    return;
  }
  vk::AttachmentDescription color = maybe_color.value();

  EXPECT_EQ(color.initialLayout, vk::ImageLayout::eUndefined);
  EXPECT_EQ(color.finalLayout, vk::ImageLayout::eShaderReadOnlyOptimal);
  EXPECT_EQ(color.loadOp, vk::AttachmentLoadOp::eClear);
  EXPECT_EQ(color.storeOp, vk::AttachmentStoreOp::eStore);

  std::optional<vk::AttachmentDescription> maybe_depth_stencil =
      builder.GetDepthStencil();
  ASSERT_TRUE(maybe_depth_stencil.has_value());
  if (!maybe_depth_stencil.has_value()) {
    return;
  }
  vk::AttachmentDescription depth_stencil = maybe_depth_stencil.value();

  EXPECT_EQ(depth_stencil.initialLayout, vk::ImageLayout::eUndefined);
  EXPECT_EQ(depth_stencil.finalLayout,
            vk::ImageLayout::eDepthStencilAttachmentOptimal);
  EXPECT_EQ(depth_stencil.loadOp, vk::AttachmentLoadOp::eDontCare);
  EXPECT_EQ(depth_stencil.storeOp, vk::AttachmentStoreOp::eDontCare);
  EXPECT_EQ(depth_stencil.stencilLoadOp, vk::AttachmentLoadOp::eDontCare);
  EXPECT_EQ(depth_stencil.stencilStoreOp, vk::AttachmentStoreOp::eDontCare);
}

// Red before EN46a: deps[0] covered only color-attachment output, so a pass's
// depth/stencil clear was not ordered after an earlier pass's depth/stencil
// writes to a shared transient attachment.
TEST(RenderPassBuilder, IncomingDependencyCoversDepthStencil) {
  RenderPassBuilderVK builder = RenderPassBuilderVK();
  auto const context = MockVulkanContextBuilder().Build();

  builder.SetColorAttachment(0, PixelFormat::kR8G8B8A8UNormInt,
                             SampleCount::kCount4, LoadAction::kClear,
                             StoreAction::kMultisampleResolve);
  builder.SetDepthStencilAttachment(PixelFormat::kD24UnormS8Uint,
                                    SampleCount::kCount4, LoadAction::kClear,
                                    StoreAction::kDontCare);

  auto render_pass = builder.Build(context->GetDevice());
  ASSERT_TRUE(!!render_pass);

  const auto& dependencies = GetLastRenderPassDependencies();
  ASSERT_EQ(dependencies.size(), 3u);
  const VkSubpassDependency& incoming = dependencies[0];
  EXPECT_EQ(incoming.srcSubpass, VK_SUBPASS_EXTERNAL);
  EXPECT_EQ(incoming.dstSubpass, 0u);

  constexpr VkPipelineStageFlags kFragmentTests =
      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  constexpr VkAccessFlags kDepthStencilReadWrite =
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  EXPECT_EQ(incoming.srcStageMask & kFragmentTests, kFragmentTests);
  EXPECT_EQ(incoming.dstStageMask & kFragmentTests, kFragmentTests);
  EXPECT_NE(
      incoming.srcAccessMask & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      0u);
  EXPECT_EQ(incoming.dstAccessMask & kDepthStencilReadWrite,
            kDepthStencilReadWrite);

  // The color scope is unchanged.
  EXPECT_NE(
      incoming.srcStageMask & VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      0u);
  EXPECT_NE(
      incoming.dstStageMask & VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      0u);
  EXPECT_NE(incoming.dstAccessMask & VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0u);
}

// Passes without a depth/stencil attachment gain no new serialization.
TEST(RenderPassBuilder, IncomingDependencyWithoutDepthStencilIsColorOnly) {
  RenderPassBuilderVK builder = RenderPassBuilderVK();
  auto const context = MockVulkanContextBuilder().Build();

  builder.SetColorAttachment(0, PixelFormat::kR8G8B8A8UNormInt,
                             SampleCount::kCount1, LoadAction::kClear,
                             StoreAction::kStore);

  auto render_pass = builder.Build(context->GetDevice());
  ASSERT_TRUE(!!render_pass);

  const auto& dependencies = GetLastRenderPassDependencies();
  ASSERT_EQ(dependencies.size(), 3u);
  EXPECT_EQ(dependencies[0].srcStageMask,
            VkPipelineStageFlags{VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT});
  EXPECT_EQ(
      dependencies[0].dstStageMask,
      VkPipelineStageFlags{VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT});
  EXPECT_EQ(dependencies[0].srcAccessMask,
            VkAccessFlags{VK_ACCESS_SHADER_READ_BIT |
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_TRANSFER_READ_BIT |
                          VK_ACCESS_TRANSFER_WRITE_BIT});
}

TEST(RenderPassBuilder, ReusedTileResolveOrdersTransferReadsAndWrites) {
  auto context = MockVulkanContextBuilder().Build();
  RenderPassBuilderVK builder;
  builder.SetColorAttachment(0, PixelFormat::kR8G8B8A8UNormInt,
                             SampleCount::kCount4, LoadAction::kClear,
                             StoreAction::kMultisampleResolve);
  ASSERT_TRUE(builder.Build(context->GetDevice()));
  const auto& dependencies = GetLastRenderPassDependencies();
  ASSERT_EQ(dependencies.size(), 3u);
  const auto& incoming = dependencies[0];
  const auto& outgoing = dependencies[2];
  EXPECT_NE(incoming.srcStageMask & VK_PIPELINE_STAGE_TRANSFER_BIT, 0u);
  EXPECT_NE(incoming.srcAccessMask & VK_ACCESS_TRANSFER_READ_BIT, 0u);
  EXPECT_NE(incoming.srcAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT, 0u);
  EXPECT_NE(outgoing.dstStageMask & VK_PIPELINE_STAGE_TRANSFER_BIT, 0u);
  EXPECT_NE(outgoing.dstAccessMask & VK_ACCESS_TRANSFER_READ_BIT, 0u);
  EXPECT_NE(outgoing.dstAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT, 0u);
  // Transfer copies can use different source/destination pixel coordinates;
  // framebuffer-local BY_REGION dependencies would be insufficient.
  EXPECT_EQ(incoming.dependencyFlags & VK_DEPENDENCY_BY_REGION_BIT, 0u);
  EXPECT_EQ(outgoing.dependencyFlags & VK_DEPENDENCY_BY_REGION_BIT, 0u);
}

TEST(RenderPassBuilder, CreatesRenderPassWithOnlyStencil) {
  RenderPassBuilderVK builder = RenderPassBuilderVK();
  auto const context = MockVulkanContextBuilder().Build();

  // Create a single color attachment with a transient depth stencil.
  builder.SetColorAttachment(0, PixelFormat::kR8G8B8A8UNormInt,
                             SampleCount::kCount1, LoadAction::kClear,
                             StoreAction::kStore);
  builder.SetStencilAttachment(PixelFormat::kS8UInt, SampleCount::kCount1,
                               LoadAction::kDontCare, StoreAction::kDontCare);

  auto render_pass = builder.Build(context->GetDevice());

  EXPECT_TRUE(!!render_pass);

  std::optional<vk::AttachmentDescription> maybe_depth_stencil =
      builder.GetDepthStencil();
  ASSERT_TRUE(maybe_depth_stencil.has_value());
  if (!maybe_depth_stencil.has_value()) {
    return;
  }
  vk::AttachmentDescription depth_stencil = maybe_depth_stencil.value();

  EXPECT_EQ(depth_stencil.initialLayout, vk::ImageLayout::eUndefined);
  EXPECT_EQ(depth_stencil.finalLayout,
            vk::ImageLayout::eDepthStencilAttachmentOptimal);
  EXPECT_EQ(depth_stencil.loadOp, vk::AttachmentLoadOp::eDontCare);
  EXPECT_EQ(depth_stencil.storeOp, vk::AttachmentStoreOp::eDontCare);
  EXPECT_EQ(depth_stencil.stencilLoadOp, vk::AttachmentLoadOp::eDontCare);
  EXPECT_EQ(depth_stencil.stencilStoreOp, vk::AttachmentStoreOp::eDontCare);
}

TEST(RenderPassBuilder, CreatesMSAAResolveWithCorrectStore) {
  RenderPassBuilderVK builder = RenderPassBuilderVK();
  auto const context = MockVulkanContextBuilder().Build();

  // Create an MSAA color attachment.
  builder.SetColorAttachment(0, PixelFormat::kR8G8B8A8UNormInt,
                             SampleCount::kCount4, LoadAction::kClear,
                             StoreAction::kMultisampleResolve);

  auto render_pass = builder.Build(context->GetDevice());

  EXPECT_TRUE(!!render_pass);

  auto maybe_color = builder.GetColor0();
  ASSERT_TRUE(maybe_color.has_value());
  if (!maybe_color.has_value()) {
    return;
  }
  vk::AttachmentDescription color = maybe_color.value();

  // MSAA Texture.
  EXPECT_EQ(color.initialLayout, vk::ImageLayout::eUndefined);
  EXPECT_EQ(color.finalLayout, vk::ImageLayout::eGeneral);
  EXPECT_EQ(color.loadOp, vk::AttachmentLoadOp::eClear);
  EXPECT_EQ(color.storeOp, vk::AttachmentStoreOp::eDontCare);

  auto maybe_resolve = builder.GetColor0Resolve();
  ASSERT_TRUE(maybe_resolve.has_value());
  if (!maybe_resolve.has_value()) {
    return;
  }
  vk::AttachmentDescription resolve = maybe_resolve.value();

  // MSAA Resolve Texture.
  EXPECT_EQ(resolve.initialLayout, vk::ImageLayout::eUndefined);
  EXPECT_EQ(resolve.finalLayout, vk::ImageLayout::eShaderReadOnlyOptimal);
  EXPECT_EQ(resolve.loadOp, vk::AttachmentLoadOp::eClear);
  EXPECT_EQ(resolve.storeOp, vk::AttachmentStoreOp::eStore);
}

}  // namespace testing
}  // namespace impeller
