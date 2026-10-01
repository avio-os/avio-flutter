// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <vector>

#include "flutter/testing/testing.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "impeller/core/formats.h"
#include "impeller/entity/entity_pass_target.h"
#include "impeller/entity/entity_playground.h"
#include "impeller/entity/render_target_cache.h"
#include "impeller/renderer/testing/mocks.h"

namespace impeller {
namespace testing {

using EntityPassTargetTest = EntityPlayground;
INSTANTIATE_PLAYGROUND_SUITE(EntityPassTargetTest);

TEST_P(EntityPassTargetTest, SwapWithMSAATexture) {
  if (GetContentContext()
          .GetDeviceCapabilities()
          .SupportsImplicitResolvingMSAA()) {
    GTEST_SKIP() << "Implicit MSAA is used on this device.";
  }
  ContentContext& content_context = GetContentContext();
  auto buffer = content_context.GetContext()->CreateCommandBuffer();
  auto render_target =
      GetContentContext().GetRenderTargetCache()->CreateOffscreenMSAA(
          *content_context.GetContext(), {100, 100},
          /*mip_count=*/1);

  auto entity_pass_target = EntityPassTarget(render_target, false, false);

  auto color0 = entity_pass_target.GetRenderTarget().GetColorAttachment(0);
  auto msaa_tex = color0.texture;
  auto resolve_tex = color0.resolve_texture;

  entity_pass_target.Flip(content_context);

  color0 = entity_pass_target.GetRenderTarget().GetColorAttachment(0);

  ASSERT_EQ(msaa_tex, color0.texture);
  ASSERT_NE(resolve_tex, color0.resolve_texture);
}

TEST_P(EntityPassTargetTest, SwapWithMSAAImplicitResolve) {
  ContentContext& content_context = GetContentContext();
  auto buffer = content_context.GetContext()->CreateCommandBuffer();
  auto context = content_context.GetContext();
  auto& allocator = *context->GetResourceAllocator();

  // Emulate implicit MSAA resolve by making color resolve and msaa texture the
  // same.
  RenderTarget render_target;
  {
    PixelFormat pixel_format =
        context->GetCapabilities()->GetDefaultColorFormat();

    // Create MSAA color texture.

    TextureDescriptor color0_tex_desc;
    color0_tex_desc.storage_mode = StorageMode::kDevicePrivate;
    color0_tex_desc.type = TextureType::kTexture2DMultisample;
    color0_tex_desc.sample_count = SampleCount::kCount4;
    color0_tex_desc.format = pixel_format;
    color0_tex_desc.size = ISize{100, 100};
    color0_tex_desc.usage = TextureUsage::kRenderTarget;

    auto color0_msaa_tex = allocator.CreateTexture(color0_tex_desc);

    // Color attachment.

    ColorAttachment color0;
    color0.load_action = LoadAction::kDontCare;
    color0.store_action = StoreAction::kStoreAndMultisampleResolve;
    color0.texture = color0_msaa_tex;
    color0.resolve_texture = color0_msaa_tex;

    render_target.SetColorAttachment(color0, 0u);
    render_target.SetStencilAttachment(std::nullopt);
  }

  auto entity_pass_target = EntityPassTarget(render_target, false, true);

  auto color0 = entity_pass_target.GetRenderTarget().GetColorAttachment(0);
  auto msaa_tex = color0.texture;
  auto resolve_tex = color0.resolve_texture;

  ASSERT_EQ(msaa_tex, resolve_tex);

  entity_pass_target.Flip(content_context);

  color0 = entity_pass_target.GetRenderTarget().GetColorAttachment(0);

  ASSERT_NE(msaa_tex, color0.texture);
  ASSERT_NE(resolve_tex, color0.resolve_texture);
  ASSERT_EQ(color0.texture, color0.resolve_texture);
}

// A ContentContext over mocks: no GPU, no pipelines. Every texture the
// render-target cache allocates is recorded, so a test can assert exactly
// what a Flip requests.
class EntityPassTargetFlipTest : public ::testing::Test {
 protected:
  void SetUp() override {
    using ::testing::_;
    using ::testing::Return;
    using ::testing::ReturnRef;
    context_ = std::make_shared<::testing::NiceMock<MockImpellerContext>>();
    allocator_ = std::make_shared<::testing::NiceMock<MockAllocator>>();
    auto capabilities =
        std::make_shared<::testing::NiceMock<MockCapabilities>>();
    capabilities_ = capabilities;
    ON_CALL(*capabilities, GetDefaultDepthStencilFormat())
        .WillByDefault(Return(PixelFormat::kD24UnormS8Uint));
    ON_CALL(*capabilities, GetDefaultColorFormat())
        .WillByDefault(Return(PixelFormat::kR8G8B8A8UNormInt));
    ON_CALL(*context_, GetCapabilities())
        .WillByDefault(ReturnRef(capabilities_));
    ON_CALL(*context_, GetResourceAllocator())
        .WillByDefault(Return(allocator_));
    ON_CALL(*allocator_, GetMaxTextureSizeSupported())
        .WillByDefault(Return(ISize{4096, 4096}));
    ON_CALL(*allocator_, OnCreateBuffer(_)).WillByDefault([](const auto& desc) {
      return std::make_shared<::testing::NiceMock<MockDeviceBuffer>>(desc);
    });
    ON_CALL(*allocator_, OnCreateTexture(_, _))
        .WillByDefault([this](const TextureDescriptor& desc, bool) {
          created_.push_back(desc);
          auto texture =
              std::make_shared<::testing::NiceMock<MockTexture>>(desc);
          ON_CALL(*texture, GetSize()).WillByDefault(Return(desc.size));
          ON_CALL(*texture, IsValid()).WillByDefault(Return(true));
          return texture;
        });
    // An invalid context skips eager pipeline setup.
    ON_CALL(*context_, IsValid()).WillByDefault(Return(false));
    content_ = std::make_unique<ContentContext>(
        context_, nullptr, std::make_shared<RenderTargetCache>(allocator_));
  }

  RenderTarget MakeMSAATarget(
      std::optional<PixelFormat> format = std::nullopt) {
    auto& cache = *content_->GetRenderTargetCache();
    cache.Start();
    RenderTarget target = cache.CreateOffscreenMSAA(
        *context_, {100, 100}, 1, "EntityPass",
        RenderTarget::kDefaultColorAttachmentConfigMSAA,
        RenderTarget::kDefaultStencilAttachmentConfig, nullptr, nullptr,
        nullptr, format);
    created_.clear();
    return target;
  }

  std::shared_ptr<::testing::NiceMock<MockImpellerContext>> context_;
  std::shared_ptr<::testing::NiceMock<MockAllocator>> allocator_;
  std::shared_ptr<const Capabilities> capabilities_;
  std::unique_ptr<ContentContext> content_;
  std::vector<TextureDescriptor> created_;
};

// Red before EN48: Flip requested a full MSAA target with depth/stencil only
// to keep its resolve texture (three textures).
TEST_F(EntityPassTargetFlipTest, FlipAllocatesOneSingleSampleTexture) {
  const RenderTarget target = MakeMSAATarget();
  ASSERT_TRUE(target.IsValid());
  EntityPassTarget pass_target(target, /*supports_read_from_resolve=*/false,
                               /*supports_implicit_msaa=*/false);
  ASSERT_TRUE(pass_target.Flip(*content_));

  ASSERT_EQ(created_.size(), 1u);
  const TextureDescriptor& secondary = created_.front();
  EXPECT_EQ(secondary.sample_count, SampleCount::kCount1);
  EXPECT_EQ(secondary.type, TextureType::kTexture2D);
  EXPECT_EQ(secondary.size, ISize(100, 100));
  EXPECT_EQ(secondary.format, PixelFormat::kR8G8B8A8UNormInt);
  EXPECT_EQ(secondary.usage,
            TextureUsage::kRenderTarget | TextureUsage::kShaderRead);
  EXPECT_EQ(
      secondary.storage_mode,
      RenderTarget::kDefaultColorAttachmentConfigMSAA.resolve_storage_mode);
}

TEST_F(EntityPassTargetFlipTest, SecondFlipAllocatesNothing) {
  EntityPassTarget pass_target(MakeMSAATarget(), false, false);
  ASSERT_TRUE(pass_target.Flip(*content_));
  ASSERT_TRUE(pass_target.Flip(*content_));
  ASSERT_TRUE(pass_target.Flip(*content_));
  EXPECT_EQ(created_.size(), 1u);
}

// The swap contract is unchanged: the multisample texture stays, the resolve
// textures alternate, and Flip returns the previous backdrop.
TEST_F(EntityPassTargetFlipTest, FlipSwapAndReturnUnchanged) {
  EntityPassTarget pass_target(MakeMSAATarget(), false, false);
  const ColorAttachment before =
      pass_target.GetRenderTarget().GetColorAttachment(0);

  const auto first_backdrop = pass_target.Flip(*content_);
  const ColorAttachment after_first =
      pass_target.GetRenderTarget().GetColorAttachment(0);
  EXPECT_EQ(first_backdrop, before.resolve_texture);
  EXPECT_EQ(after_first.texture, before.texture);
  EXPECT_NE(after_first.resolve_texture, before.resolve_texture);

  const auto second_backdrop = pass_target.Flip(*content_);
  const ColorAttachment after_second =
      pass_target.GetRenderTarget().GetColorAttachment(0);
  EXPECT_EQ(second_backdrop, after_first.resolve_texture);
  EXPECT_EQ(after_second.resolve_texture, before.resolve_texture);
  EXPECT_EQ(after_second.texture, before.texture);
}

// Red before EN48: the secondary took the context's default format, not the
// resolve texture's.
TEST_F(EntityPassTargetFlipTest, FlipKeepsNonDefaultResolveFormat) {
  EntityPassTarget pass_target(MakeMSAATarget(PixelFormat::kR16G16B16A16Float),
                               false, false);
  ASSERT_TRUE(pass_target.Flip(*content_));
  ASSERT_EQ(created_.size(), 1u);
  EXPECT_EQ(created_.front().format, PixelFormat::kR16G16B16A16Float);
  EXPECT_EQ(pass_target.GetRenderTarget()
                .GetColorAttachment(0)
                .resolve_texture->GetTextureDescriptor()
                .format,
            PixelFormat::kR16G16B16A16Float);
}

}  // namespace testing
}  // namespace impeller
