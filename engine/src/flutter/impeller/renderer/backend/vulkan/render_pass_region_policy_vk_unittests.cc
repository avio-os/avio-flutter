// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gtest/gtest.h"
#include "impeller/renderer/backend/vulkan/render_pass_builder_vk.h"

namespace impeller {
namespace testing {

// The cache samples the native four ordered coverage samples. A resolve cannot
// protect those samples when the native attachment transitions from UNDEFINED.
TEST(RenderPassRegionPolicyVK, PartialClearPreservesNativeMaskSamples) {
  RenderPassBuilderVK builder;
  builder.SetColorAttachment(
      0, PixelFormat::kR8UNormInt, SampleCount::kCount4, LoadAction::kClear,
      StoreAction::kStore, vk::ImageLayout::eShaderReadOnlyOptimal, false,
      std::nullopt, vk::ImageLayout::eShaderReadOnlyOptimal);
  ASSERT_TRUE(builder.GetColor0());
  EXPECT_EQ(builder.GetColor0()->initialLayout,
            vk::ImageLayout::eShaderReadOnlyOptimal);
  EXPECT_EQ(builder.GetColor0()->loadOp, vk::AttachmentLoadOp::eClear);
  EXPECT_EQ(builder.GetColor0()->storeOp, vk::AttachmentStoreOp::eStore);
  EXPECT_EQ(builder.GetColor0()->samples, vk::SampleCountFlagBits::e4);
  EXPECT_FALSE(builder.GetColor0Resolve());
}

TEST(RenderPassRegionPolicyVK, PartialColourResolvePreservesBothImages) {
  RenderPassBuilderVK builder;
  builder.SetColorAttachment(
      0, PixelFormat::kB8G8R8A8UNormInt, SampleCount::kCount4,
      LoadAction::kClear, StoreAction::kMultisampleResolve,
      vk::ImageLayout::eGeneral, false, vk::ImageLayout::eShaderReadOnlyOptimal,
      vk::ImageLayout::eGeneral);
  ASSERT_TRUE(builder.GetColor0());
  ASSERT_TRUE(builder.GetColor0Resolve());
  EXPECT_EQ(builder.GetColor0()->initialLayout, vk::ImageLayout::eGeneral);
  EXPECT_EQ(builder.GetColor0()->loadOp, vk::AttachmentLoadOp::eClear);
  EXPECT_EQ(builder.GetColor0Resolve()->initialLayout,
            vk::ImageLayout::eShaderReadOnlyOptimal);
  EXPECT_EQ(builder.GetColor0Resolve()->loadOp,
            vk::AttachmentLoadOp::eDontCare);
  EXPECT_EQ(builder.GetColor0Resolve()->storeOp, vk::AttachmentStoreOp::eStore);
}

TEST(RenderPassRegionPolicyVK, ColdFullClearCanInitializeUndefinedImage) {
  RenderPassBuilderVK builder;
  builder.SetColorAttachment(0, PixelFormat::kR8UNormInt, SampleCount::kCount4,
                             LoadAction::kClear, StoreAction::kStore);
  ASSERT_TRUE(builder.GetColor0());
  EXPECT_EQ(builder.GetColor0()->initialLayout, vk::ImageLayout::eUndefined);
  EXPECT_EQ(builder.GetColor0()->loadOp, vk::AttachmentLoadOp::eClear);
  EXPECT_EQ(builder.GetColor0()->storeOp, vk::AttachmentStoreOp::eStore);
  EXPECT_FALSE(builder.GetColor0Resolve());
}

TEST(RenderPassRegionPolicyVK,
     NativeMaskPreservationDoesNotRequireStencilLoad) {
  RenderPassBuilderVK builder;
  builder.SetColorAttachment(0, PixelFormat::kR8UNormInt, SampleCount::kCount4,
                             LoadAction::kClear, StoreAction::kStore,
                             vk::ImageLayout::eGeneral, false, std::nullopt,
                             vk::ImageLayout::eGeneral);
  builder.SetStencilAttachment(PixelFormat::kS8UInt, SampleCount::kCount4,
                               LoadAction::kClear, StoreAction::kDontCare);
  ASSERT_TRUE(builder.GetColor0());
  ASSERT_TRUE(builder.GetDepthStencil());
  EXPECT_EQ(builder.GetColor0()->initialLayout, vk::ImageLayout::eGeneral);
  EXPECT_EQ(builder.GetDepthStencil()->stencilLoadOp,
            vk::AttachmentLoadOp::eClear);
  EXPECT_EQ(builder.GetDepthStencil()->initialLayout,
            vk::ImageLayout::eUndefined);
}

}  // namespace testing
}  // namespace impeller
