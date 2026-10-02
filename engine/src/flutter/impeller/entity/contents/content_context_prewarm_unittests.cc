// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Pipeline variants that ContentContext compiles at construction (Avio patch
// 55), so that the first use of a variant on Avio's SDF, color source and
// backdrop paths does not compile it on the raster thread.

#include <algorithm>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "impeller/core/formats.h"
#include "impeller/core/raw_ptr.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/contents/contents.h"
#include "impeller/entity/entity.h"
#include "impeller/entity/entity_playground.h"
#include "impeller/renderer/pipeline_descriptor.h"
#include "impeller/renderer/render_target.h"
#include "impeller/renderer/testing/mocks.h"

namespace impeller {
namespace testing {

namespace {

using Pipeline = PrewarmVariant::Pipeline;

// Avio's Vulkan Shell: RGBA offscreens, BGRA (DRM ARGB8888) roots, 4x MSAA
// save layers and roots, storage buffers.
PrewarmTargets AvioVulkanTargets() {
  return PrewarmTargets{
      .offscreen_format = PixelFormat::kR8G8B8A8UNormInt,
      .root_format = PixelFormat::kB8G8R8A8UNormInt,
      .multisampled_passes = true,
      .supports_ssbo = true,
  };
}

ContentContextOptions Options(SampleCount sample_count,
                              PixelFormat format,
                              bool has_depth_stencil_attachments,
                              BlendMode blend_mode,
                              PrimitiveType primitive_type,
                              bool depth_write_enabled) {
  return ContentContextOptions{
      .sample_count = sample_count,
      .blend_mode = blend_mode,
      .depth_compare = CompareFunction::kGreaterEqual,
      .stencil_mode = ContentContextOptions::StencilMode::kIgnore,
      .primitive_type = primitive_type,
      .color_attachment_pixel_format = format,
      .has_depth_stencil_attachments = has_depth_stencil_attachments,
      .depth_write_enabled = depth_write_enabled,
  };
}

bool Contains(const std::vector<PrewarmVariant>& variants,
              Pipeline pipeline,
              const ContentContextOptions& options) {
  return std::find(variants.begin(), variants.end(),
                   PrewarmVariant{.pipeline = pipeline, .options = options}) !=
         variants.end();
}

size_t CountPipeline(const std::vector<PrewarmVariant>& variants,
                     Pipeline pipeline) {
  return static_cast<size_t>(
      std::count_if(variants.begin(), variants.end(),
                    [pipeline](const PrewarmVariant& variant) {
                      return variant.pipeline == pipeline;
                    }));
}

bool HasDuplicates(const std::vector<PrewarmVariant>& variants) {
  for (size_t i = 0; i < variants.size(); i++) {
    for (size_t j = i + 1; j < variants.size(); j++) {
      if (variants[i] == variants[j]) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

// Patch 50 draws every SDF shape with UberSDF's storage-buffer variant, into
// save layers (RGBA) and the root (BGRA), both 4x with depth/stencil; patch
// 52 snapshots its mask single-sample without depth/stencil.
TEST(ContentContextPrewarmSetTest, UberSDFIsPrewarmedForEveryPassItDrawsIn) {
  const std::vector<PrewarmVariant> variants =
      MakeAvioPrewarmVariants(AvioVulkanTargets());

  EXPECT_EQ(CountPipeline(variants, Pipeline::kUberSDFSSBO), 3u);
  EXPECT_EQ(CountPipeline(variants, Pipeline::kUberSDF), 0u);
  EXPECT_TRUE(Contains(
      variants, Pipeline::kUberSDFSSBO,
      Options(SampleCount::kCount4, PixelFormat::kR8G8B8A8UNormInt, true,
              BlendMode::kSrcOver, PrimitiveType::kTriangleStrip, false)));
  EXPECT_TRUE(Contains(
      variants, Pipeline::kUberSDFSSBO,
      Options(SampleCount::kCount4, PixelFormat::kB8G8R8A8UNormInt, true,
              BlendMode::kSrcOver, PrimitiveType::kTriangleStrip, false)));
  EXPECT_TRUE(Contains(
      variants, Pipeline::kUberSDFSSBO,
      Options(SampleCount::kCount1, PixelFormat::kR8G8B8A8UNormInt, false,
              BlendMode::kSrcOver, PrimitiveType::kTriangleStrip, false)));
}

// Single-sample variants are exactly patch 52's snapshots and blend target:
// offscreen format, no depth/stencil. Everything Canvas draws into a pass is
// multisampled with depth/stencil.
TEST(ContentContextPrewarmSetTest, SampleCountsMatchThePassesTheyDrawIn) {
  const std::vector<PrewarmVariant> variants =
      MakeAvioPrewarmVariants(AvioVulkanTargets());

  size_t single_sample = 0;
  for (const PrewarmVariant& variant : variants) {
    if (variant.options.sample_count == SampleCount::kCount1) {
      single_sample++;
      EXPECT_FALSE(variant.options.has_depth_stencil_attachments);
      EXPECT_EQ(variant.options.color_attachment_pixel_format,
                PixelFormat::kR8G8B8A8UNormInt);
    } else {
      EXPECT_EQ(variant.options.sample_count, SampleCount::kCount4);
      EXPECT_TRUE(variant.options.has_depth_stencil_attachments);
    }
  }
  EXPECT_EQ(single_sample, 5u);
  EXPECT_EQ(variants.size(), 27u);
  EXPECT_FALSE(HasDuplicates(variants));
}

// Patch 52's "Pipeline Blend Filter" draws the mask with kSrc and the color
// source with kSrcIn into a single-sample target; the composite and the
// backdrop restored after patch 48's Flip are textures in the 4x passes.
TEST(ContentContextPrewarmSetTest, BlendFilterAndFlipRestoreTextures) {
  const std::vector<PrewarmVariant> variants =
      MakeAvioPrewarmVariants(AvioVulkanTargets());

  for (const BlendMode blend_mode : {BlendMode::kSrc, BlendMode::kSrcIn}) {
    EXPECT_TRUE(Contains(
        variants, Pipeline::kTexture,
        Options(SampleCount::kCount1, PixelFormat::kR8G8B8A8UNormInt, false,
                blend_mode, PrimitiveType::kTriangleStrip, false)));
  }
  for (const PixelFormat format :
       {PixelFormat::kR8G8B8A8UNormInt, PixelFormat::kB8G8R8A8UNormInt}) {
    for (const BlendMode blend_mode : {BlendMode::kSrcOver, BlendMode::kSrc}) {
      EXPECT_TRUE(Contains(variants, Pipeline::kTexture,
                           Options(SampleCount::kCount4, format, true,
                                   blend_mode, PrimitiveType::kTriangleStrip,
                                   /*depth_write_enabled=*/false)));
    }
  }
  EXPECT_EQ(CountPipeline(variants, Pipeline::kTexture), 6u);
}

// Patch 52 (a) draws an image or gradient on a rect that contains the clip
// directly in the pass; an opaque one draws with kSrc and writes depth.
TEST(ContentContextPrewarmSetTest, ColorSourcesOnARectContainingTheClip) {
  const std::vector<PrewarmVariant> variants =
      MakeAvioPrewarmVariants(AvioVulkanTargets());

  for (const PixelFormat format :
       {PixelFormat::kR8G8B8A8UNormInt, PixelFormat::kB8G8R8A8UNormInt}) {
    for (const bool opaque : {false, true}) {
      const BlendMode blend_mode =
          opaque ? BlendMode::kSrc : BlendMode::kSrcOver;
      EXPECT_TRUE(
          Contains(variants, Pipeline::kTiledTexture,
                   Options(SampleCount::kCount4, format, true, blend_mode,
                           PrimitiveType::kTriangleStrip, opaque)));
      EXPECT_TRUE(
          Contains(variants, Pipeline::kFastGradient,
                   Options(SampleCount::kCount4, format, true, blend_mode,
                           PrimitiveType::kTriangle, opaque)));
      EXPECT_TRUE(
          Contains(variants, Pipeline::kLinearGradientSSBOFill,
                   Options(SampleCount::kCount4, format, true, blend_mode,
                           PrimitiveType::kTriangleStrip, opaque)));
      EXPECT_TRUE(
          Contains(variants, Pipeline::kRadialGradientSSBOFill,
                   Options(SampleCount::kCount4, format, true, blend_mode,
                           PrimitiveType::kTriangleStrip, opaque)));
    }
  }
  // A gradient UberSDF cannot shade goes through patch 52's single-sample
  // color source snapshot.
  for (const Pipeline pipeline :
       {Pipeline::kLinearGradientSSBOFill, Pipeline::kRadialGradientSSBOFill}) {
    EXPECT_TRUE(Contains(
        variants, pipeline,
        Options(SampleCount::kCount1, PixelFormat::kR8G8B8A8UNormInt, false,
                BlendMode::kSrcOver, PrimitiveType::kTriangleStrip, false)));
  }
}

// Backends without storage buffers draw UberSDF with its ramp texture and
// shade gradients with uniform or texture pipelines, which are not
// prewarmed.
TEST(ContentContextPrewarmSetTest, RampTextureBackendPrewarmsUberSDF) {
  PrewarmTargets targets = AvioVulkanTargets();
  targets.supports_ssbo = false;
  const std::vector<PrewarmVariant> variants = MakeAvioPrewarmVariants(targets);

  EXPECT_EQ(CountPipeline(variants, Pipeline::kUberSDF), 3u);
  EXPECT_EQ(CountPipeline(variants, Pipeline::kUberSDFSSBO), 0u);
  EXPECT_EQ(CountPipeline(variants, Pipeline::kLinearGradientSSBOFill), 0u);
  EXPECT_EQ(CountPipeline(variants, Pipeline::kRadialGradientSSBOFill), 0u);
  EXPECT_TRUE(Contains(
      variants, Pipeline::kUberSDF,
      Options(SampleCount::kCount1, PixelFormat::kR8G8B8A8UNormInt, false,
              BlendMode::kSrcOver, PrimitiveType::kTriangleStrip, false)));
  EXPECT_EQ(variants.size(), 17u);
}

// Without offscreen MSAA, save layers and the root are single-sample but keep
// depth/stencil; the snapshots are unchanged.
TEST(ContentContextPrewarmSetTest, SingleSamplePassesKeepDepthStencil) {
  PrewarmTargets targets = AvioVulkanTargets();
  targets.multisampled_passes = false;
  const std::vector<PrewarmVariant> variants = MakeAvioPrewarmVariants(targets);

  EXPECT_TRUE(Contains(
      variants, Pipeline::kUberSDFSSBO,
      Options(SampleCount::kCount1, PixelFormat::kB8G8R8A8UNormInt, true,
              BlendMode::kSrcOver, PrimitiveType::kTriangleStrip, false)));
  EXPECT_TRUE(Contains(
      variants, Pipeline::kUberSDFSSBO,
      Options(SampleCount::kCount1, PixelFormat::kR8G8B8A8UNormInt, false,
              BlendMode::kSrcOver, PrimitiveType::kTriangleStrip, false)));
  for (const PrewarmVariant& variant : variants) {
    EXPECT_EQ(variant.options.sample_count, SampleCount::kCount1);
  }
  EXPECT_EQ(variants.size(), 27u);
}

// A root in the offscreen format shares its variants.
TEST(ContentContextPrewarmSetTest, RootInTheOffscreenFormatAddsNothing) {
  PrewarmTargets targets = AvioVulkanTargets();
  targets.root_format = targets.offscreen_format;
  const std::vector<PrewarmVariant> variants = MakeAvioPrewarmVariants(targets);

  EXPECT_EQ(variants.size(), 16u);
  EXPECT_FALSE(HasDuplicates(variants));
  for (const PrewarmVariant& variant : variants) {
    EXPECT_EQ(variant.options.color_attachment_pixel_format,
              PixelFormat::kR8G8B8A8UNormInt);
  }
}

namespace {

// Builds the render targets Avio draws into over mocks and computes options
// with the same functions the draw sites call.
class ContentContextPrewarmOptionsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    using ::testing::_;
    using ::testing::Return;
    using ::testing::ReturnRef;
    context_ = std::make_shared<::testing::NiceMock<MockImpellerContext>>();
    allocator_ = std::make_shared<::testing::NiceMock<MockAllocator>>();
    capabilities_ = std::make_shared<::testing::NiceMock<MockCapabilities>>();
    ON_CALL(*capabilities_, GetDefaultDepthStencilFormat())
        .WillByDefault(Return(PixelFormat::kD24UnormS8Uint));
    ON_CALL(*capabilities_, GetDefaultColorFormat())
        .WillByDefault(Return(PixelFormat::kR8G8B8A8UNormInt));
    ON_CALL(*capabilities_, SupportsOffscreenMSAA())
        .WillByDefault(Return(true));
    ON_CALL(*capabilities_, SupportsSSBO()).WillByDefault(Return(true));
    const_capabilities_ = capabilities_;
    ON_CALL(*context_, GetCapabilities())
        .WillByDefault(ReturnRef(const_capabilities_));
    ON_CALL(*context_, GetResourceAllocator())
        .WillByDefault(Return(allocator_));
    ON_CALL(*allocator_, GetMaxTextureSizeSupported())
        .WillByDefault(Return(ISize{4096, 4096}));
    ON_CALL(*allocator_, OnCreateTexture(_, _))
        .WillByDefault([](const TextureDescriptor& desc, bool) {
          auto texture =
              std::make_shared<::testing::NiceMock<MockTexture>>(desc);
          ON_CALL(*texture, GetSize()).WillByDefault(Return(desc.size));
          ON_CALL(*texture, IsValid()).WillByDefault(Return(true));
          return texture;
        });
  }

  std::unique_ptr<RenderPass> MakePass(const RenderTarget& target) {
    return std::make_unique<::testing::NiceMock<MockRenderPass>>(context_,
                                                                 target);
  }

  // ColorSourceContents::DrawGeometry: UberSDF, gradients and images.
  static ContentContextOptions DrawGeometryOptions(
      const RenderPass& pass,
      BlendMode blend_mode,
      PrimitiveType primitive_type) {
    Entity entity;
    entity.SetBlendMode(blend_mode);
    ContentContextOptions options = OptionsFromPassAndEntity(pass, entity);
    options.primitive_type = primitive_type;
    options.depth_write_enabled = options.blend_mode == BlendMode::kSrc;
    return options;
  }

  // TextureContents::Render: the masked composite (stencil enabled) and the
  // backdrop restored after a Flip (stencil disabled).
  static ContentContextOptions TextureContentsOptions(const RenderPass& pass,
                                                      BlendMode blend_mode,
                                                      bool stencil_enabled) {
    Entity entity;
    entity.SetBlendMode(blend_mode);
    ContentContextOptions options = OptionsFromPassAndEntity(pass, entity);
    if (!stencil_enabled) {
      options.stencil_mode = ContentContextOptions::StencilMode::kIgnore;
    }
    options.primitive_type = PrimitiveType::kTriangleStrip;
    options.depth_write_enabled =
        stencil_enabled && options.blend_mode == BlendMode::kSrc;
    return options;
  }

  // PipelineBlend in blend_filter_contents.cc.
  static ContentContextOptions PipelineBlendOptions(const RenderPass& pass,
                                                    BlendMode blend_mode) {
    ContentContextOptions options = OptionsFromPass(pass);
    options.primitive_type = PrimitiveType::kTriangleStrip;
    options.blend_mode = blend_mode;
    return options;
  }

  std::shared_ptr<::testing::NiceMock<MockImpellerContext>> context_;
  std::shared_ptr<::testing::NiceMock<MockAllocator>> allocator_;
  std::shared_ptr<::testing::NiceMock<MockCapabilities>> capabilities_;
  std::shared_ptr<const Capabilities> const_capabilities_;
};

}  // namespace

TEST_F(ContentContextPrewarmOptionsTest, AvioTargetsComeFromTheCapabilities) {
  const PrewarmTargets targets = MakeAvioPrewarmTargets(*capabilities_);
  EXPECT_EQ(targets.offscreen_format, PixelFormat::kR8G8B8A8UNormInt);
  EXPECT_EQ(targets.root_format, PixelFormat::kB8G8R8A8UNormInt);
  EXPECT_TRUE(targets.multisampled_passes);
  EXPECT_TRUE(targets.supports_ssbo);
}

// The set is exactly the options the draw sites compute in the passes Avio
// renders into, from the same OptionsFromPass the draws start from.
TEST_F(ContentContextPrewarmOptionsTest, SetIsWhatTheDrawSitesCompute) {
  RenderTargetAllocator allocator(allocator_);
  // Canvas save layers (CreateRenderTarget): 4x in the offscreen format.
  const RenderTarget save_layer =
      allocator.CreateOffscreenMSAA(*context_, ISize{64, 64}, 1);
  // Avio's root: 4x BGRA with depth/stencil.
  const RenderTarget root = allocator.CreateOffscreenMSAA(
      *context_, ISize{64, 64}, 1, "Root",
      RenderTarget::kDefaultColorAttachmentConfigMSAA,
      RenderTarget::kDefaultStencilAttachmentConfig,
      /*existing_color_msaa_texture=*/nullptr,
      /*existing_color_resolve_texture=*/nullptr,
      /*existing_depth_stencil_texture=*/nullptr,
      /*target_pixel_format=*/PixelFormat::kB8G8R8A8UNormInt);
  // ContentContext::MakeSubpass with MSAA and depth/stencil disabled: patch
  // 52's snapshots and the "Pipeline Blend Filter" target.
  const RenderTarget snapshot =
      allocator.CreateOffscreen(*context_, ISize{64, 64}, 1, "Snapshot",
                                RenderTarget::kDefaultColorAttachmentConfig,
                                /*stencil_attachment_config=*/std::nullopt);
  ASSERT_TRUE(save_layer.IsValid());
  ASSERT_TRUE(root.IsValid());
  ASSERT_TRUE(snapshot.IsValid());

  std::vector<PrewarmVariant> expected;
  const auto expect = [&expected](Pipeline pipeline,
                                  const ContentContextOptions& options) {
    if (std::find(expected.begin(), expected.end(),
                  PrewarmVariant{.pipeline = pipeline, .options = options}) ==
        expected.end()) {
      expected.push_back({.pipeline = pipeline, .options = options});
    }
  };
  for (const RenderTarget* target : {&save_layer, &root}) {
    const std::unique_ptr<RenderPass> pass = MakePass(*target);
    expect(Pipeline::kUberSDFSSBO,
           DrawGeometryOptions(*pass, BlendMode::kSrcOver,
                               PrimitiveType::kTriangleStrip));
    expect(Pipeline::kTexture,
           TextureContentsOptions(*pass, BlendMode::kSrcOver,
                                  /*stencil_enabled=*/true));
    expect(Pipeline::kTexture,
           TextureContentsOptions(*pass, BlendMode::kSrc,
                                  /*stencil_enabled=*/false));
    for (const BlendMode blend_mode : {BlendMode::kSrcOver, BlendMode::kSrc}) {
      expect(Pipeline::kTiledTexture,
             DrawGeometryOptions(*pass, blend_mode,
                                 PrimitiveType::kTriangleStrip));
      expect(Pipeline::kFastGradient,
             DrawGeometryOptions(*pass, blend_mode, PrimitiveType::kTriangle));
      expect(Pipeline::kLinearGradientSSBOFill,
             DrawGeometryOptions(*pass, blend_mode,
                                 PrimitiveType::kTriangleStrip));
      expect(Pipeline::kRadialGradientSSBOFill,
             DrawGeometryOptions(*pass, blend_mode,
                                 PrimitiveType::kTriangleStrip));
    }
  }
  {
    const std::unique_ptr<RenderPass> pass = MakePass(snapshot);
    expect(Pipeline::kUberSDFSSBO,
           DrawGeometryOptions(*pass, BlendMode::kSrcOver,
                               PrimitiveType::kTriangleStrip));
    expect(Pipeline::kTexture, PipelineBlendOptions(*pass, BlendMode::kSrc));
    expect(Pipeline::kTexture, PipelineBlendOptions(*pass, BlendMode::kSrcIn));
    expect(Pipeline::kLinearGradientSSBOFill,
           DrawGeometryOptions(*pass, BlendMode::kSrcOver,
                               PrimitiveType::kTriangleStrip));
    expect(Pipeline::kRadialGradientSSBOFill,
           DrawGeometryOptions(*pass, BlendMode::kSrcOver,
                               PrimitiveType::kTriangleStrip));
  }

  const std::vector<PrewarmVariant> variants =
      MakeAvioPrewarmVariants(MakeAvioPrewarmTargets(*capabilities_));
  EXPECT_EQ(variants.size(), expected.size());
  for (const PrewarmVariant& variant : expected) {
    EXPECT_TRUE(Contains(variants, variant.pipeline, variant.options))
        << "pipeline " << static_cast<int>(variant.pipeline) << " key "
        << variant.options.ToKey();
  }
}

namespace {

// The getter each draw site calls for `variant`'s pipeline.
PipelineRef GetVariantPipeline(const ContentContext& renderer,
                               const PrewarmVariant& variant) {
  switch (variant.pipeline) {
    case Pipeline::kUberSDF:
      return renderer.GetUberSDFPipeline(variant.options);
    case Pipeline::kUberSDFSSBO:
      return renderer.GetUberSDFSSBOPipeline(variant.options);
    case Pipeline::kTexture:
      return renderer.GetTexturePipeline(variant.options);
    case Pipeline::kTiledTexture:
      return renderer.GetTiledTexturePipeline(variant.options);
    case Pipeline::kFastGradient:
      return renderer.GetFastGradientPipeline(variant.options);
    case Pipeline::kLinearGradientSSBOFill:
      return renderer.GetLinearGradientSSBOFillPipeline(variant.options);
    case Pipeline::kRadialGradientSSBOFill:
      return renderer.GetRadialGradientSSBOFillPipeline(variant.options);
  }
}

bool IsPrewarmed(std::string_view label) {
  return label.ends_with(" Prewarmed");
}

}  // namespace

using ContentContextPrewarmTest = EntityPlayground;
INSTANTIATE_PLAYGROUND_SUITE(ContentContextPrewarmTest);

// Red without the prewarm: the first use of each variant compiles it on the
// calling thread, under the lazy "V#n" label. With it, the draw site's getter
// returns the pipeline queued at construction, and that pipeline is built for
// exactly the pass its options name.
TEST_P(ContentContextPrewarmTest, FirstUseFindsThePrewarmedVariant) {
  if (!EnsureContextUsesSDFs()) {
    GTEST_SKIP() << "Only contexts that render with SDFs prewarm.";
  }
  ContentContext renderer(GetContext(), GetTypographerContext());
  ASSERT_TRUE(renderer.IsValid());

  const std::vector<PrewarmVariant> variants = MakeAvioPrewarmVariants(
      MakeAvioPrewarmTargets(*GetContext()->GetCapabilities()));
  ASSERT_FALSE(variants.empty());
  for (const PrewarmVariant& variant : variants) {
    PipelineRef pipeline = GetVariantPipeline(renderer, variant);
    ASSERT_TRUE(pipeline) << "pipeline " << static_cast<int>(variant.pipeline)
                          << " key " << variant.options.ToKey();
    const PipelineDescriptor& desc = pipeline->GetDescriptor();
    EXPECT_TRUE(IsPrewarmed(desc.GetLabel())) << desc.GetLabel();
    EXPECT_EQ(desc.GetSampleCount(), variant.options.sample_count);
    ASSERT_NE(desc.GetColorAttachmentDescriptor(0u), nullptr);
    EXPECT_EQ(desc.GetColorAttachmentDescriptor(0u)->format,
              variant.options.color_attachment_pixel_format);
    EXPECT_EQ(desc.GetDepthStencilAttachmentDescriptor().has_value(),
              variant.options.has_depth_stencil_attachments);
    EXPECT_EQ(desc.GetPrimitiveType(), variant.options.primitive_type);
  }

  // A variant outside the set is still compiled on first use.
  ContentContextOptions outside = variants.back().options;
  outside.blend_mode = BlendMode::kXor;
  PipelineRef lazy = renderer.GetTexturePipeline(outside);
  ASSERT_TRUE(lazy);
  EXPECT_FALSE(IsPrewarmed(lazy->GetDescriptor().GetLabel()));
}

// Contexts without SDFs (Avio's GTK engine, stock Flutter) keep the stock
// startup: nothing beyond the defaults is queued.
TEST_P(ContentContextPrewarmTest, ContextsWithoutSDFsDoNotPrewarm) {
  if (GetContext()->GetFlags().use_sdfs) {
    GTEST_SKIP() << "This backend renders with SDFs.";
  }
  ContentContext renderer(GetContext(), GetTypographerContext());
  ASSERT_TRUE(renderer.IsValid());

  const ContentContextOptions snapshot_blend =
      Options(SampleCount::kCount1,
              GetContext()->GetCapabilities()->GetDefaultColorFormat(), false,
              BlendMode::kSrcIn, PrimitiveType::kTriangleStrip, false);
  PipelineRef pipeline = renderer.GetTexturePipeline(snapshot_blend);
  ASSERT_TRUE(pipeline);
  EXPECT_FALSE(IsPrewarmed(pipeline->GetDescriptor().GetLabel()));
}

}  // namespace testing
}  // namespace impeller
