// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/sample4_clip_uniform.h"

#include "gtest/gtest.h"

namespace impeller {
namespace {

TEST(Sample4ClipUniformTest, JointPredicateKeepsPhysicalGridAndDifference) {
  auto quad = CoverageConvexQuad4::Make(
      Rect::MakeLTRB(4.25, 8.5, 20.75, 32.125).GetPoints());
  ASSERT_TRUE(quad);
  AvioSample4ClipDescriptor clip;
  clip.count = 2;
  clip.nodes[0].quad = quad;
  clip.nodes[1].quad = quad;
  clip.nodes[1].operation = ClipOperation::kDifference;
  auto uniform = MakeAvioSample4ClipUniform(clip, {224, 448});
  ASSERT_TRUE(uniform);
  EXPECT_EQ(uniform->runtime[0], 2.f);
  EXPECT_EQ(uniform->origin[0], 224.f);
  EXPECT_EQ(uniform->origin[1], 448.f);
  EXPECT_EQ(uniform->nodes[0].meta[1], 0.f);
  EXPECT_EQ(uniform->nodes[1].meta[1], 1.f);
  for (size_t i = 0; i < 16; ++i) {
    EXPECT_EQ(uniform->nodes[0].lines.m[i], quad->GetLineParameters().m[i]);
  }
}

TEST(Sample4ClipUniformTest, DeepRecipeCannotSilentlyLoseParentPredicate) {
  auto quad =
      CoverageConvexQuad4::Make(Rect::MakeSize(Size(20, 20)).GetPoints());
  ASSERT_TRUE(quad);
  auto parent = std::make_shared<AvioSample4ClipDescriptor>();
  parent->count = 1;
  parent->nodes[0].quad = quad;
  AvioSample4ClipDescriptor leaf;
  leaf.count = 1;
  leaf.nodes[0].quad = quad;
  leaf.parent = parent;
  // The caller must flatten all ancestors into one unresolved native4 tile;
  // uploading only the newest K4 bundle would incorrectly fail open.
  EXPECT_FALSE(MakeAvioSample4ClipUniform(leaf, {}));
}

TEST(Sample4ClipUniformTest, RefusesMissingAmbiguousAndUnknownRepresentations) {
  AvioSample4ClipDescriptor clip;
  EXPECT_FALSE(MakeAvioSample4ClipUniform(clip, {}));
  clip.count = 1;
  EXPECT_FALSE(MakeAvioSample4ClipUniform(clip, {}));
  auto quad =
      CoverageConvexQuad4::Make(Rect::MakeSize(Size(20, 20)).GetPoints());
  ASSERT_TRUE(quad);
  clip.nodes[0].quad = quad;
  clip.nodes[0].mask = CoverageMaskTile{};
  EXPECT_FALSE(MakeAvioSample4ClipUniform(clip, {}));
  clip.nodes[0].mask.reset();
  clip.nodes[0].operation = static_cast<ClipOperation>(100);
  EXPECT_FALSE(MakeAvioSample4ClipUniform(clip, {}));
  clip.nodes[0].operation = ClipOperation::kIntersect;
  clip.count = 5;
  EXPECT_FALSE(MakeAvioSample4ClipUniform(clip, {}));
}

}  // namespace
}  // namespace impeller
