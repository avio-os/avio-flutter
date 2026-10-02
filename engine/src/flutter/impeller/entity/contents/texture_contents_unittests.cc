// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/texture_contents.h"

#include "gtest/gtest.h"

namespace impeller::testing {

TEST(TextureContentsSampling, HalfTexelInsetRetainsExactSourceView) {
  const auto source = Rect::MakeXYWH(37.25f, 11.5f, 12, 8);
  const auto clamp = TextureContents::GetSourceSamplingBounds(source);
  EXPECT_EQ(clamp, Rect::MakeLTRB(37.75f, 12, 48.75f, 19));
  EXPECT_TRUE(source.Contains(clamp));
}

TEST(TextureContentsSampling, SubtexelCropKeepsClampOrderedAtItsCenter) {
  const auto source = Rect::MakeXYWH(37.25f, 11.5f, .25f, .75f);
  const auto clamp = TextureContents::GetSourceSamplingBounds(source);
  EXPECT_EQ(clamp.GetLeft(), source.GetCenter().x);
  EXPECT_EQ(clamp.GetRight(), source.GetCenter().x);
  EXPECT_EQ(clamp.GetTop(), source.GetCenter().y);
  EXPECT_EQ(clamp.GetBottom(), source.GetCenter().y);
  // Projecting a point clamp through the full physical texture keeps that
  // exact source texel location, rather than turning it into an empty origin.
  const auto uv = Rect::MakeSize(ISize(128, 64)).Project(clamp);
  EXPECT_FLOAT_EQ(uv.GetLeft(), source.GetCenter().x / 128);
  EXPECT_FLOAT_EQ(uv.GetBottom(), source.GetCenter().y / 64);
}

TEST(TextureContentsSampling, ThinAxisDoesNotCollapseTheOtherAxis) {
  const auto source = Rect::MakeXYWH(37.25f, 11.5f, .25f, 8);
  const auto clamp = TextureContents::GetSourceSamplingBounds(source);
  EXPECT_EQ(clamp.GetLeft(), clamp.GetRight());
  EXPECT_EQ(clamp.GetTop(), 12);
  EXPECT_EQ(clamp.GetBottom(), 19);
}

TEST(TextureContentsSampling, CapturedSourceRequiresCompleteImmutableEvidence) {
  const auto destination = Rect::MakeWH(64, 64);
  const auto source = Rect::MakeXYWH(16, 16, 64, 64);
  const auto opaque = Rect::MakeWH(128, 128);
  const auto clip = Rect::MakeXYWH(4.25, 4.25, 55.5, 55.5);
  const auto raster = IRect::MakeXYWH(3, 3, 58, 58);
  const auto proof = TextureContents::MakeCapturedSample4SourceProof(
      destination, source, Matrix(), opaque, clip, raster, 17, 1.f, true);
  ASSERT_TRUE(proof);
  EXPECT_TRUE(proof->Matches(17));
  EXPECT_FALSE(proof->Matches(18));
  EXPECT_TRUE(proof->captured_backdrop);
  EXPECT_TRUE(proof->source_is_opaque);
  EXPECT_FALSE(TextureContents::MakeCapturedSample4SourceProof(
      destination, source, Matrix(), opaque, clip, raster, 17, 1.f, false));
  const auto translucent = TextureContents::MakeCapturedSample4SourceProof(
      destination, source, Matrix(), opaque, clip, raster, 17, .999f, true);
  ASSERT_TRUE(translucent);
  EXPECT_FALSE(translucent->source_is_opaque);
}

TEST(TextureContentsSampling, CapturedFringeCannotReadOutsideOpaquePrefix) {
  const auto destination = Rect::MakeWH(64, 64);
  const auto source = Rect::MakeXYWH(16, 16, 64, 64);
  const auto clip = Rect::MakeXYWH(.25, .25, 63.5, 63.5);
  const auto raster = IRect::MakeSize(ISize(64, 64));
  // Source may cover the exact clip lanes yet its linear neighbourhood can
  // touch unknown/transparent texels. That is not a group opacity proof.
  const auto uncertain_alpha = TextureContents::MakeCapturedSample4SourceProof(
      destination, source, Matrix(), source, clip, raster, 17, 1.f, true);
  ASSERT_TRUE(uncertain_alpha);
  EXPECT_FALSE(uncertain_alpha->source_is_opaque);
  EXPECT_TRUE(uncertain_alpha->full_clip_geometry);
  EXPECT_FALSE(TextureContents::MakeCapturedSample4SourceProof(
      Rect::MakeWH(32, 64), source, Matrix(), Rect::MakeWH(128, 128), clip,
      raster, 17, 1.f, true));
}

TEST(TextureContentsSampling, ImmutableNormalGaussianNeedsNoOpaqueAssumption) {
  const auto proof = TextureContents::MakeCapturedSample4SourceProof(
      Rect::MakeWH(64, 64), Rect::MakeXYWH(16, 16, 64, 64), Matrix(),
      std::nullopt, Rect::MakeXYWH(4.25, 4.25, 55.5, 55.5),
      IRect::MakeXYWH(3, 3, 58, 58), 17, 1.f, true);
  ASSERT_TRUE(proof);
  EXPECT_TRUE(proof->captured_backdrop);
  EXPECT_TRUE(proof->uniform_samples);
  EXPECT_FALSE(proof->source_is_opaque);
  EXPECT_FALSE(proof->source_is_encoded);
}

}  // namespace impeller::testing
