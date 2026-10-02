// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gtest/gtest.h"
#include "impeller/core/formats.h"
#include "impeller/entity/inline_pass_context.h"
#include "impeller/renderer/testing/mocks.h"

namespace impeller {
namespace testing {

namespace {
class ClassifiedScopeContext : public ::testing::NiceMock<MockImpellerContext> {
 public:
  AvioAntialiasingConfig policy;
  const AvioAntialiasingConfig& GetAvioAntialiasingConfig() const override {
    return policy;
  }
};

class InlinePassClassifiedScopeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    using ::testing::_;
    using ::testing::Return;
    using ::testing::ReturnRef;
    context = std::make_shared<ClassifiedScopeContext>();
    context->policy.policy = AvioAntialiasingPolicy::kCoverage;
    allocator = std::make_shared<::testing::NiceMock<MockAllocator>>();
    capabilities = std::make_shared<::testing::NiceMock<MockCapabilities>>();
    ON_CALL(*context, GetCapabilities()).WillByDefault(ReturnRef(capabilities));
    ON_CALL(*context, GetResourceAllocator()).WillByDefault(Return(allocator));
    // Keep native pipeline initialization outside this source-only fixture.
    ON_CALL(*context, IsValid()).WillByDefault(Return(false));
    renderer = std::make_unique<ContentContext>(context, nullptr);
    TextureDescriptor desc;
    desc.size = {64, 64};
    desc.format = PixelFormat::kR8G8B8A8UNormInt;
    auto texture = std::make_shared<::testing::NiceMock<MockTexture>>(desc);
    ON_CALL(*texture, IsValid()).WillByDefault(Return(true));
    ON_CALL(*texture, GetSize()).WillByDefault(Return(desc.size));
    ColorAttachment colour;
    colour.texture = texture;
    colour.load_action = LoadAction::kLoad;
    parent.SetColorAttachment(colour, 0);
    target = std::make_unique<EntityPassTarget>(parent, false, false);
    command = std::make_shared<::testing::NiceMock<MockCommandBuffer>>(context);
    ON_CALL(*command, IsValid()).WillByDefault(Return(true));
    ON_CALL(*context, CreateCommandBuffer()).WillByDefault(Return(command));
    auto native =
        std::make_shared<::testing::NiceMock<MockRenderPass>>(context, parent);
    ON_CALL(*native, IsValid()).WillByDefault(Return(true));
    // A terminal encode failure lets the fixture verify failure/late-proof
    // admission without creating or submitting a real GPU command buffer.
    ON_CALL(*native, OnEncodeCommands(_)).WillByDefault(Return(false));
    ON_CALL(*command, OnCreateRenderPass(_)).WillByDefault(Return(native));
    scope = std::make_unique<InlinePassContext>(*renderer, *target, true);
  }

  std::shared_ptr<ClassifiedScopeContext> context;
  std::shared_ptr<::testing::NiceMock<MockAllocator>> allocator;
  std::shared_ptr<const Capabilities> capabilities;
  std::unique_ptr<ContentContext> renderer;
  std::shared_ptr<::testing::NiceMock<MockCommandBuffer>> command;
  RenderTarget parent;
  std::unique_ptr<EntityPassTarget> target;
  std::unique_ptr<InlinePassContext> scope;
};
}  // namespace

TEST_F(InlinePassClassifiedScopeTest,
       ProofSelectsSingleSampleBeforePassAndNeverChangesMidScope) {
  ASSERT_TRUE(scope->SetAvioDirect1xProof(true));
  const auto& pass = scope->GetRenderPass();
  ASSERT_NE(pass, nullptr);
  EXPECT_EQ(pass->GetSampleCount(), SampleCount::kCount1);
  EXPECT_FALSE(pass->HasDepthAttachment());
  EXPECT_FALSE(pass->HasStencilAttachment());
  EXPECT_EQ(pass->GetRenderTarget().GetColorAttachment(0).load_action,
            LoadAction::kLoad);
  EXPECT_FALSE(scope->SetAvioDirect1xProof(false));
  EXPECT_FALSE(scope->EndPass());
  EXPECT_FALSE(scope->SetAvioDirect1xProof(true));
}

TEST_F(InlinePassClassifiedScopeTest,
       LegacyAndAnyRequestedContinuousClassRefuseDirectProof) {
  context->policy.policy = AvioAntialiasingPolicy::kMsaa4;
  EXPECT_FALSE(scope->SetAvioDirect1xProof(true));
  EXPECT_TRUE(scope->SetAvioDirect1xProof(false));
  context->policy.policy = AvioAntialiasingPolicy::kCoverage;
  context->policy.continuous_requested_classes = 1;
  EXPECT_FALSE(scope->SetAvioDirect1xProof(true));
  EXPECT_TRUE(scope->SetAvioDirect1xProof(false));
  context->policy.continuous_requested_classes = 0;
  EXPECT_TRUE(scope->SetAvioDirect1xProof(true));
}

// Every target a save layer or subpass paints into comes from
// `RenderTargetCache`, declaring `kDontCare` because nothing chose a load
// action for it. It is recycled, so it arrives holding the previous tenant's
// pixels. Honoring the declaration ghosts them through wherever this pass does
// not paint.
TEST(InlinePassContextTest, FirstPassClearsARecycledTarget) {
  EXPECT_EQ(ColorLoadActionForPass(/*pass_count=*/0, /*is_msaa=*/false,
                                   LoadAction::kDontCare,
                                   /*honor_declared_load_action=*/false),
            LoadAction::kClear);
  EXPECT_EQ(ColorLoadActionForPass(/*pass_count=*/0, /*is_msaa=*/true,
                                   LoadAction::kDontCare,
                                   /*honor_declared_load_action=*/false),
            LoadAction::kClear);
}

// A target that did not opt in cannot vouch for itself by declaring `kLoad`
// either: the opt-in is the caller's statement, not the target's.
TEST(InlinePassContextTest, FirstPassClearsEvenWhenTheTargetDeclaresLoad) {
  EXPECT_EQ(ColorLoadActionForPass(/*pass_count=*/0, /*is_msaa=*/false,
                                   LoadAction::kLoad,
                                   /*honor_declared_load_action=*/false),
            LoadAction::kClear);
}

// The root pass over an embedder-supplied target does opt in, which is what
// lets partial repaint keep the pixels outside the damage region.
TEST(InlinePassContextTest, FirstPassKeepsTheDeclaredActionWhenHonored) {
  EXPECT_EQ(ColorLoadActionForPass(/*pass_count=*/0, /*is_msaa=*/false,
                                   LoadAction::kLoad,
                                   /*honor_declared_load_action=*/true),
            LoadAction::kLoad);
  EXPECT_EQ(ColorLoadActionForPass(/*pass_count=*/0, /*is_msaa=*/false,
                                   LoadAction::kClear,
                                   /*honor_declared_load_action=*/true),
            LoadAction::kClear);
  EXPECT_EQ(ColorLoadActionForPass(/*pass_count=*/0, /*is_msaa=*/false,
                                   LoadAction::kDontCare,
                                   /*honor_declared_load_action=*/true),
            LoadAction::kDontCare);
}

// Later passes continue a target this context already painted. A single-sample
// attachment loads it; a fresh MSAA attachment has nothing to load and must be
// cleared before it resolves over the same resolve texture.
TEST(InlinePassContextTest, LaterPassesContinueTheTargetTheyAlreadyPainted) {
  for (bool honored : {false, true}) {
    EXPECT_EQ(ColorLoadActionForPass(/*pass_count=*/1, /*is_msaa=*/false,
                                     LoadAction::kDontCare, honored),
              LoadAction::kLoad);
    EXPECT_EQ(ColorLoadActionForPass(/*pass_count=*/1, /*is_msaa=*/true,
                                     LoadAction::kDontCare, honored),
              LoadAction::kClear);
  }
}

}  // namespace testing
}  // namespace impeller
