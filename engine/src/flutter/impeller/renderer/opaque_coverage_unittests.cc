// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/opaque_coverage.h"

#include "gtest/gtest.h"

namespace impeller {
namespace {

TEST(OpaqueCoverage, LoadNeverCertifiesPreviousTenantOrOpaqueClearMetadata) {
  OpaqueCoverageState state;
  const auto bounds = Rect::MakeWH(128, 64);
  state.RecordClear(LoadAction::kLoad, 1.f, bounds);
  EXPECT_FALSE(state.GetOpaqueRect());
  state.RecordClear(LoadAction::kDontCare, 1.f, bounds);
  EXPECT_FALSE(state.GetOpaqueRect());
  state.RecordClear(LoadAction::kClear, .999f, bounds);
  EXPECT_FALSE(state.GetOpaqueRect());
  state.RecordClear(LoadAction::kClear, 1.f, bounds);
  EXPECT_EQ(state.GetOpaqueRect(), bounds);
}

TEST(OpaqueCoverage, SrcOverAndProvenSrcPreserveExactOpaquePrefix) {
  OpaqueCoverageState state;
  const auto bounds = Rect::MakeWH(128, 64);
  state.RecordClear(LoadAction::kClear, 1.f, bounds);
  state.RecordDraw(BlendMode::kSrcOver);
  EXPECT_EQ(state.GetOpaqueRect(), bounds);
  state.RecordDraw(BlendMode::kSrc, std::nullopt, true);
  EXPECT_EQ(state.GetOpaqueRect(), bounds);
  state.RecordDraw(BlendMode::kSrc);
  EXPECT_FALSE(state.GetOpaqueRect());
  state.RecordDraw(BlendMode::kSrc, bounds);
  EXPECT_EQ(state.GetOpaqueRect(), bounds);
}

TEST(OpaqueCoverage, AlphaReducingOrUnknownOperatorsInvalidateEvidence) {
  for (const auto blend :
       {BlendMode::kClear, BlendMode::kDstIn, BlendMode::kSrcIn,
        BlendMode::kXor, BlendMode::kScreen}) {
    OpaqueCoverageState state;
    state.RecordClear(LoadAction::kClear, 1.f, Rect::MakeWH(128, 64));
    state.RecordDraw(blend);
    EXPECT_FALSE(state.GetOpaqueRect());
  }
}

TEST(OpaqueCoverage, DisjointWritesDoNotInventOpaqueUnion) {
  OpaqueCoverageState state;
  const auto first = Rect::MakeXYWH(0, 0, 8, 8);
  state.RecordDraw(BlendMode::kSrc, first);
  state.RecordDraw(BlendMode::kSrcOver, Rect::MakeXYWH(16, 0, 8, 8));
  EXPECT_EQ(state.GetOpaqueRect(), first);
  EXPECT_FALSE(state.GetOpaqueRect()->Contains(Rect::MakeXYWH(8, 0, 8, 8)));
}

TEST(OpaqueCoverage, FilterProofErodesEveryTapAndLinearNeighbour) {
  const auto uvs = Rect::MakeWH(1, 1).GetPoints();
  const auto result = MapOpaqueSamplingFootprint(
      Rect::MakeWH(128, 64), ISize(128, 64), uvs,
      IRect::MakeXYWH(16, 32, 64, 32), Vector2(3.5f, 1.5f));
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, Rect::MakeLTRB(18, 33, 78, 63));
  // The output's physical origin is retained; no logical-size texture fiction.
  EXPECT_FALSE(result->Contains(Rect::MakeXYWH(16, 32, 1, 1)));
}

TEST(OpaqueCoverage, GaussianPassChainCannotCertifyDecalHalo) {
  const auto full_uvs = Rect::MakeWH(1, 1).GetPoints();
  auto downsample = MapOpaqueSamplingFootprint(
      Rect::MakeWH(128, 128), ISize(128, 128), full_uvs,
      IRect::MakeSize(ISize(64, 64)), Vector2(3.5, 3.5));
  ASSERT_TRUE(downsample);
  auto vertical = MapOpaqueSamplingFootprint(
      *downsample, ISize(64, 64), full_uvs, IRect::MakeSize(ISize(64, 64)),
      Vector2(.5, 4.5));
  ASSERT_TRUE(vertical);
  auto horizontal = MapOpaqueSamplingFootprint(
      *vertical, ISize(64, 64), full_uvs, IRect::MakeSize(ISize(64, 64)),
      Vector2(4.5, .5));
  ASSERT_TRUE(horizontal);
  EXPECT_EQ(*horizontal, Rect::MakeLTRB(8, 8, 56, 56));
  EXPECT_FALSE(horizontal->Contains(Rect::MakeXYWH(0, 0, 8, 8)));
}

TEST(OpaqueCoverage, RotatedOrDegenerateSamplingMapsStayUnknown) {
  auto uvs = Rect::MakeWH(1, 1).GetPoints();
  uvs[1].y = .25f;
  EXPECT_FALSE(MapOpaqueSamplingFootprint(Rect::MakeWH(128, 64), ISize(128, 64),
                                          uvs, IRect::MakeSize(ISize(64, 32)),
                                          Vector2(.5f, .5f)));
  EXPECT_FALSE(MapOpaqueSamplingFootprint(
      Rect::MakeWH(1, 1), ISize(128, 64), Rect::MakeWH(1, 1).GetPoints(),
      IRect::MakeSize(ISize(64, 32)), Vector2(1.f, 1.f)));
}

}  // namespace
}  // namespace impeller
