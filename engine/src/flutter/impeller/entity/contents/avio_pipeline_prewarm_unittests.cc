// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <set>
#include <vector>
#include "gtest/gtest.h"
#include "impeller/entity/contents/avio_pipeline_initialization.h"
#include "impeller/entity/contents/avio_pipeline_prewarm.h"
namespace impeller {
namespace testing {
namespace {
using Family = AvioPrewarmPipeline;
using Storage = AvioPrewarmGradientStorage;
using Stencil = ContentContextOptions::StencilMode;
struct Key {
  Family family;
  ContentContextOptions options;
  Storage storage;
  ConicalKind conical;
};
std::vector<Key> Catalogue(bool ssbo,
                           bool sdfs = true,
                           bool stencil_only = false) {
  std::vector<Key> keys;
  const AvioPipelinePrewarmConfig config{
      .native_format = PixelFormat::kB8G8R8A8UNormInt,
      .native_stencil_only = stencil_only,
      .supports_ssbo = ssbo,
      .supports_triangle_fan = true,
      .use_sdfs = sdfs};
  EXPECT_TRUE(PrewarmAvioPipelineKeys(
      config, [&](Family family, const ContentContextOptions& options,
                  Storage storage, ConicalKind conical) {
        keys.push_back({family, options, storage, conical});
        return true;
      }));
  return keys;
}
bool Has(const std::vector<Key>& keys,
         Family family,
         const ContentContextOptions& options) {
  for (const auto& key : keys) {
    if (key.family == family && key.options.ToKey() == options.ToKey())
      return true;
  }
  return false;
}
TEST(AvioPipelineInitializationTest, ColdFactoriesRunBeforePublicReadiness) {
  AvioPipelineInitialization state;
  EXPECT_FALSE(state.IsReady());
  EXPECT_FALSE(state.CanCreatePipelines());
  {
    AvioPipelineInitialization::Scope cold(state);
    EXPECT_FALSE(state.IsReady());
    EXPECT_TRUE(state.CanCreatePipelines());
    size_t successful_keys = 0;
    EXPECT_TRUE(PrewarmAvioPipelineKeys(
        {PixelFormat::kR8G8B8A8UNormInt},
        [&](Family, const ContentContextOptions&, Storage, ConicalKind) {
          EXPECT_FALSE(state.IsReady());
          EXPECT_TRUE(state.CanCreatePipelines());
          successful_keys++;
          return true;
        }));
    EXPECT_GT(successful_keys, 0u);
    cold.Complete();
  }
  EXPECT_TRUE(state.IsReady());
  EXPECT_TRUE(state.CanCreatePipelines());
}
TEST(AvioPipelineInitializationTest, FailedColdFactoryNeverBecomesValid) {
  AvioPipelineInitialization state;
  size_t attempts = 0;
  {
    AvioPipelineInitialization::Scope cold(state);
    EXPECT_FALSE(PrewarmAvioPipelineKeys(
        {PixelFormat::kR8G8B8A8UNormInt},
        [&](Family, const ContentContextOptions&, Storage, ConicalKind) {
          return ++attempts != 7;
        }));
  }
  EXPECT_EQ(attempts, 7u);
  EXPECT_FALSE(state.IsReady());
  EXPECT_FALSE(state.CanCreatePipelines());
}
TEST(AvioPipelinePrewarmTest, QueuesAllKeysBeforeJoiningTheSameKeys) {
  const AvioPipelinePrewarmConfig config{
      .native_format = PixelFormat::kB8G8R8A8UNormInt,
      .supports_ssbo = true,
      .supports_triangle_fan = true,
      .use_sdfs = true};
  const auto expected = Catalogue(true);
  std::vector<Key> queued;
  size_t joined = 0;
  EXPECT_TRUE(PrewarmAvioPipelineKeysInTwoPhases(
      config,
      [&](Family family, const ContentContextOptions& options, Storage storage,
          ConicalKind conical) {
        EXPECT_EQ(joined, 0u);
        queued.push_back({family, options, storage, conical});
        return true;
      },
      [&](Family family, const ContentContextOptions& options, Storage storage,
          ConicalKind conical) {
        EXPECT_EQ(queued.size(), expected.size());
        if (joined >= queued.size()) {
          return false;
        }
        const auto& key = queued[joined++];
        EXPECT_EQ(family, key.family);
        EXPECT_EQ(options.ToKey(), key.options.ToKey());
        EXPECT_EQ(storage, key.storage);
        EXPECT_EQ(conical, key.conical);
        return true;
      }));
  EXPECT_EQ(joined, expected.size());
}
TEST(AvioPipelinePrewarmTest, QueueRefusalNeverStartsJoining) {
  size_t queued = 0;
  size_t joined = 0;
  EXPECT_FALSE(PrewarmAvioPipelineKeysInTwoPhases(
      {PixelFormat::kR8G8B8A8UNormInt},
      [&](Family, const ContentContextOptions&, Storage, ConicalKind) {
        return ++queued != 7;
      },
      [&](Family, const ContentContextOptions&, Storage, ConicalKind) {
        joined++;
        return true;
      }));
  EXPECT_EQ(queued, 7u);
  EXPECT_EQ(joined, 0u);
}
TEST(AvioPipelineInitializationTest, AsyncJoinFailureKeepsPublicStateUnready) {
  AvioPipelineInitialization state;
  size_t queued = 0;
  size_t joined = 0;
  {
    AvioPipelineInitialization::Scope cold(state);
    const bool ready = PrewarmAvioPipelineKeysInTwoPhases(
        {PixelFormat::kR8G8B8A8UNormInt},
        [&](Family, const ContentContextOptions&, Storage, ConicalKind) {
          EXPECT_FALSE(state.IsReady());
          queued++;
          return true;
        },
        [&](Family, const ContentContextOptions&, Storage, ConicalKind) {
          EXPECT_FALSE(state.IsReady());
          EXPECT_GT(queued, 7u);
          return ++joined != 7;
        });
    EXPECT_FALSE(ready);
    if (ready) {
      cold.Complete();
    }
  }
  EXPECT_EQ(joined, 7u);
  EXPECT_FALSE(state.IsReady());
  EXPECT_FALSE(state.CanCreatePipelines());
}
TEST(AvioPipelineInitializationTest, PublishesReadyOnlyAfterAllAsyncJoins) {
  AvioPipelineInitialization state;
  size_t queued = 0;
  size_t joined = 0;
  {
    AvioPipelineInitialization::Scope cold(state);
    ASSERT_TRUE(PrewarmAvioPipelineKeysInTwoPhases(
        {PixelFormat::kR8G8B8A8UNormInt},
        [&](Family, const ContentContextOptions&, Storage, ConicalKind) {
          EXPECT_FALSE(state.IsReady());
          queued++;
          return true;
        },
        [&](Family, const ContentContextOptions&, Storage, ConicalKind) {
          EXPECT_FALSE(state.IsReady());
          joined++;
          return true;
        }));
    EXPECT_GT(queued, 0u);
    EXPECT_EQ(joined, queued);
    EXPECT_FALSE(state.IsReady());
    cold.Complete();
  }
  EXPECT_TRUE(state.IsReady());
}
TEST(AvioPipelinePrewarmTest, ActualSourceKeysFollowRecordingPassAuthority) {
  const auto keys = Catalogue(true);
  ContentContextOptions options;
  options.depth_compare = CompareFunction::kGreaterEqual;
  options.primitive_type = PrimitiveType::kTriangle;
  options.has_depth_stencil_attachments = false;
  for (auto format :
       {PixelFormat::kR8G8B8A8UNormInt, PixelFormat::kB8G8R8A8UNormInt}) {
    options.color_attachment_pixel_format = format;
    // Text/gradient/shadow scopes are not approved by the direct1x classifier.
    EXPECT_FALSE(Has(keys, Family::kGlyph, options));
    EXPECT_FALSE(Has(keys, Family::kShadowVertices, options));
    options.primitive_type = PrimitiveType::kTriangleStrip;
    EXPECT_TRUE(Has(keys, Family::kUberSdf, options));
    EXPECT_FALSE(Has(keys, Family::kComplexRse, options));
    EXPECT_TRUE(Has(keys, Family::kTexture, options));
    options.primitive_type = PrimitiveType::kTriangle;
  }
  options.sample_count = SampleCount::kCount4;
  options.has_depth_stencil_attachments = true;
  EXPECT_TRUE(Has(keys, Family::kGlyph, options));
  EXPECT_TRUE(Has(keys, Family::kShadowVertices, options));
  options.blend_mode = BlendMode::kSrc;
  EXPECT_TRUE(Has(keys, Family::kGlyph, options));
  options.depth_compare = CompareFunction::kAlways;
  EXPECT_FALSE(Has(keys, Family::kGlyph, options));
  for (const auto& key : keys) {
    if (key.options.sample_count == SampleCount::kCount1) {
      EXPECT_FALSE(key.options.has_depth_stencil_attachments);
      EXPECT_EQ(key.options.stencil_mode, Stencil::kIgnore);
      EXPECT_EQ(key.options.primitive_type, PrimitiveType::kTriangleStrip);
    }
  }
}
TEST(AvioPipelinePrewarmTest, PathCoverStrokeAndCutKeysMatchTheirRealCallers) {
  const auto keys = Catalogue(true);
  ContentContextOptions options;
  options.sample_count = SampleCount::kCount4;
  options.color_attachment_pixel_format = PixelFormat::kB8G8R8A8UNormInt;
  options.depth_compare = CompareFunction::kGreaterEqual;
  options.primitive_type = PrimitiveType::kTriangleStrip;
  options.stencil_mode = Stencil::kCoverCompare;
  EXPECT_TRUE(Has(keys, Family::kLinearGradient, options));
  options.stencil_mode = Stencil::kIgnore;
  options.depth_compare = CompareFunction::kGreater;
  options.depth_write_enabled = true;
  EXPECT_TRUE(Has(keys, Family::kSolid, options));
  EXPECT_FALSE(Has(keys, Family::kTiledTexture, options));
  options.depth_compare = CompareFunction::kGreaterEqual;
  options.blend_mode = BlendMode::kSrc;
  EXPECT_TRUE(Has(keys, Family::kTexture, options));
  options.depth_write_enabled = false;
  EXPECT_TRUE(Has(keys, Family::kTexture, options));
  options.blend_mode = BlendMode::kClear;
  options.is_for_rrect_blur_clear = true;
  EXPECT_TRUE(Has(keys, Family::kRRectBlur, options));
  options.is_for_rrect_blur_clear = false;
  options.blend_mode = BlendMode::kScreen;
  EXPECT_TRUE(Has(keys, Family::kStrictTexture, options));
}
TEST(AvioPipelinePrewarmTest, ClipPreparationAndCoverWriteHaveDifferentKeys) {
  const auto keys = Catalogue(true, true, true);
  ContentContextOptions options;
  options.sample_count = SampleCount::kCount4;
  options.is_stencil_only = true;
  options.color_attachment_pixel_format = PixelFormat::kB8G8R8A8UNormInt;
  options.blend_mode = BlendMode::kDst;
  options.depth_compare = CompareFunction::kGreaterEqual;
  options.primitive_type = PrimitiveType::kTriangleFan;
  options.stencil_mode = Stencil::kStencilEvenOddFill;
  EXPECT_TRUE(Has(keys, Family::kClip, options));
  options.depth_write_enabled = true;
  EXPECT_FALSE(Has(keys, Family::kClip, options));
  options.primitive_type = PrimitiveType::kTriangleStrip;
  options.stencil_mode = Stencil::kCoverCompareInverted;
  EXPECT_TRUE(Has(keys, Family::kClip, options));
}
TEST(AvioPipelinePrewarmTest, DeviceFlagsSelectExistingShaderFamilies) {
  auto native = Catalogue(true);
  auto portable = Catalogue(false, false);
  bool uniform = false, texture = false;
  for (const auto& key : native) {
    EXPECT_NE(key.family, Family::kConicalGradient);
    EXPECT_NE(key.family, Family::kSweepGradient);
    if (key.family == Family::kLinearGradient ||
        key.family == Family::kRadialGradient) {
      EXPECT_EQ(key.storage, Storage::kSsbo);
      EXPECT_EQ(key.options.sample_count, SampleCount::kCount4);
    }
  }
  for (const auto& key : portable) {
    EXPECT_NE(key.family, Family::kUberSdf);
    EXPECT_NE(key.family, Family::kComplexRse);
    if (key.family == Family::kLinearGradient) {
      EXPECT_NE(key.storage, Storage::kSsbo);
      uniform |= key.storage == Storage::kUniform;
      texture |= key.storage == Storage::kTexture;
    }
  }
  EXPECT_TRUE(uniform);
  EXPECT_TRUE(texture);
  // A bounded catalogue rather than all pass/enum/flag combinations.
  EXPECT_LT(native.size(), 180u);
  EXPECT_LT(portable.size(), 200u);
}
TEST(AvioPipelinePrewarmTest, GlesDecalDownsampleWarmsOnlyAdmittedSubpasses) {
  size_t fallbacks = 0;
  AvioPipelinePrewarmConfig config{
      .native_format = PixelFormat::kR8G8B8A8UNormInt,
      .needs_gles_decal_downsample = true};
  EXPECT_TRUE(PrewarmAvioPipelineKeys(
      config, [&](Family family, const ContentContextOptions& options, Storage,
                  ConicalKind) {
        if (family == Family::kDownsampleGles) {
          ++fallbacks;
          EXPECT_EQ(options.sample_count, SampleCount::kCount1);
          EXPECT_FALSE(options.has_depth_stencil_attachments);
          EXPECT_EQ(options.depth_compare, CompareFunction::kGreaterEqual);
        }
        return true;
      }));
  EXPECT_EQ(fallbacks, 1u);
}
TEST(AvioPipelinePrewarmTest, MixedNativePassesKeepTheirAnalyticSourceKeys) {
  const auto enabled = Catalogue(true);
  const auto disabled = Catalogue(true, false);
  ContentContextOptions options;
  options.sample_count = SampleCount::kCount4;
  options.color_attachment_pixel_format = PixelFormat::kB8G8R8A8UNormInt;
  options.depth_compare = CompareFunction::kGreaterEqual;
  options.primitive_type = PrimitiveType::kTriangleStrip;
  for (auto blend : {BlendMode::kSrcOver, BlendMode::kSrc, BlendMode::kScreen,
                     BlendMode::kDstOut}) {
    options.blend_mode = blend;
    options.depth_write_enabled = blend == BlendMode::kSrc;
    EXPECT_TRUE(Has(enabled, Family::kUberSdf, options));
    EXPECT_TRUE(Has(enabled, Family::kComplexRse, options));
    EXPECT_FALSE(Has(disabled, Family::kUberSdf, options));
    EXPECT_FALSE(Has(disabled, Family::kComplexRse, options));
    EXPECT_TRUE(Has(disabled, Family::kCircle, options));
  }
  for (auto blend : {BlendMode::kClear, BlendMode::kDstIn}) {
    options.blend_mode = blend;
    options.depth_write_enabled = false;
    EXPECT_TRUE(Has(disabled, Family::kCircle, options));
    EXPECT_FALSE(Has(enabled, Family::kUberSdf, options));
  }
  options.sample_count = SampleCount::kCount1;
  options.has_depth_stencil_attachments = false;
  options.blend_mode = BlendMode::kSrcOver;
  EXPECT_FALSE(Has(enabled, Family::kCircle, options));
  EXPECT_FALSE(Has(enabled, Family::kComplexRse, options));
  EXPECT_TRUE(Has(enabled, Family::kUberSdf, options));
}
TEST(AvioPipelinePrewarmTest, NegotiatedSdfCutsDoNotDependOnEn50Flag) {
  bool uber_clear = false;
  const AvioPipelinePrewarmConfig config{
      .native_format = PixelFormat::kB8G8R8A8UNormInt,
      .use_sdfs = false,
      .continuous_sdf_cuts = true};
  EXPECT_TRUE(PrewarmAvioPipelineKeys(
      config, [&](Family family, const ContentContextOptions& options, Storage,
                  ConicalKind) {
        EXPECT_NE(family, Family::kComplexRse);
        uber_clear |= family == Family::kUberSdf &&
                      options.blend_mode == BlendMode::kClear &&
                      options.sample_count == SampleCount::kCount4 &&
                      options.depth_compare == CompareFunction::kGreaterEqual &&
                      !options.depth_write_enabled;
        return true;
      }));
  EXPECT_TRUE(uber_clear);
}

TEST(AvioPipelinePrewarmTest,
     ShellGradientCutsAndFastGradientUseActualTopology) {
  const auto keys = Catalogue(true);
  ContentContextOptions options;
  options.sample_count = SampleCount::kCount4;
  options.color_attachment_pixel_format = PixelFormat::kB8G8R8A8UNormInt;
  options.depth_compare = CompareFunction::kGreaterEqual;
  for (auto blend : {BlendMode::kScreen, BlendMode::kDstIn}) {
    options.blend_mode = blend;
    options.primitive_type = PrimitiveType::kTriangleStrip;
    options.stencil_mode = Stencil::kIgnore;
    EXPECT_TRUE(Has(keys, Family::kLinearGradient, options));
    options.primitive_type = PrimitiveType::kTriangle;
    EXPECT_TRUE(Has(keys, Family::kFastGradient, options));
    options.stencil_mode = Stencil::kCoverCompare;
    EXPECT_TRUE(Has(keys, Family::kFastGradient, options));
    options.primitive_type = PrimitiveType::kTriangleStrip;
    EXPECT_FALSE(Has(keys, Family::kFastGradient, options));
  }
}

TEST(AvioPipelinePrewarmTest, NoUnsupportedFanOrUnadmittedPassCrossProduct) {
  size_t attempts = 0;
  AvioPipelinePrewarmConfig config{
      .native_format = PixelFormat::kR8G8B8A8UNormInt,
      .supports_ssbo = true,
      .supports_triangle_fan = false,
      .use_sdfs = true};
  std::set<std::pair<Family, uint64_t>> unique;
  EXPECT_TRUE(PrewarmAvioPipelineKeys(
      config, [&](Family family, const ContentContextOptions& options, Storage,
                  ConicalKind) {
        attempts++;
        EXPECT_NE(options.primitive_type, PrimitiveType::kTriangleFan);
        EXPECT_TRUE(unique.emplace(family, options.ToKey()).second);
        if (options.sample_count == SampleCount::kCount1) {
          EXPECT_NE(family, Family::kClip);
          EXPECT_NE(family, Family::kTiledTexture);
          EXPECT_NE(family, Family::kLinearGradient);
          EXPECT_NE(family, Family::kRadialGradient);
          EXPECT_EQ(options.stencil_mode, Stencil::kIgnore);
        }
        if (family == Family::kUberSdf || family == Family::kComplexRse ||
            family == Family::kCircle) {
          EXPECT_EQ(options.primitive_type, PrimitiveType::kTriangleStrip);
          EXPECT_EQ(options.stencil_mode, Stencil::kIgnore);
          EXPECT_EQ(options.depth_compare, CompareFunction::kGreaterEqual);
          if (family != Family::kUberSdf) {
            EXPECT_EQ(options.sample_count, SampleCount::kCount4);
          }
        }
        return true;
      }));
  EXPECT_GT(attempts, 0u);
}

TEST(AvioPipelinePrewarmTest, InvalidNativeFormatDoesNotInvokeFactory) {
  size_t attempts = 0;
  EXPECT_FALSE(PrewarmAvioPipelineKeys(
      {PixelFormat::kUnknown},
      [&](Family, const ContentContextOptions&, Storage, ConicalKind) {
        attempts++;
        return true;
      }));
  EXPECT_EQ(attempts, 0u);
}
}  // namespace
}  // namespace testing
}  // namespace impeller
