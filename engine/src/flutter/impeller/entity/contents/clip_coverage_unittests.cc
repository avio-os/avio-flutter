// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/clip_coverage.h"

#include <limits>

#include "gtest/gtest.h"

namespace impeller {
namespace testing {

TEST(ClipCoverage, NestedMasksPreserveSampleCorrelation) {
  for (uint32_t parent = 0; parent < 256; parent++) {
    for (uint32_t shape = 0; shape < 256; shape++) {
      EXPECT_EQ(
          CombineClipSampleMask4(parent, shape, ClipOperation::kIntersect),
          (parent & shape) & 15u);
      EXPECT_EQ(
          CombineClipSampleMask4(parent, shape, ClipOperation::kDifference),
          (parent & ~shape) & 15u);
    }
  }
  // Two half-covered clips can share all their samples or none of them.
  // Multiplying resolved alpha would incorrectly produce 0.25 in both cases.
  EXPECT_EQ(ResolveClipSampleMask4(
                CombineClipSampleMask4(3, 3, ClipOperation::kIntersect)),
            0.5f);
  EXPECT_EQ(ResolveClipSampleMask4(
                CombineClipSampleMask4(3, 12, ClipOperation::kIntersect)),
            0.0f);
  EXPECT_EQ(ResolveClipSampleMask4(255), 1.0f);
}

TEST(ClipCoverage, FractionalRectUsesPhysicalStandardSamplePositions) {
  EXPECT_EQ(ClipRectSampleMask4(Rect::MakeLTRB(0, 0, 0.5, 1), {0, 0}), 5);
  EXPECT_EQ(ClipRectSampleMask4(Rect::MakeLTRB(0, 0, 1, 0.5), {0, 0}), 3);
  // A sample on the exclusive right edge is outside, on the left is inside.
  EXPECT_EQ(ClipRectSampleMask4(Rect::MakeLTRB(0.375, 0, 1, 1), {0, 0}), 11);
  EXPECT_EQ(ClipRectSampleMask4(Rect::MakeLTRB(0, 0, 0.375, 1), {0, 0}), 4);
  EXPECT_EQ(ClipRectSampleMask4(Rect::MakeLTRB(-1, -1, -0.5, 0), {-1, -1}), 5);
  EXPECT_EQ(ClipRectSampleMask4(Rect::MakeLTRB(0, 0, 0, 1), {0, 0}), 0);
}

TEST(ClipCoverage, ScissorRequiresAnExactIntegralIntersect) {
  EXPECT_TRUE(CanUseRectClipScissor4(Rect::MakeLTRB(-1, 0, 4, 3),
                                     ClipOperation::kIntersect));
  EXPECT_FALSE(CanUseRectClipScissor4(Rect::MakeLTRB(0.1, 0, 4, 3),
                                      ClipOperation::kIntersect));
  EXPECT_FALSE(CanUseRectClipScissor4(Rect::MakeLTRB(0, 0, 4, 3),
                                      ClipOperation::kDifference));
  const auto rect = Rect::MakeLTRB(0.49, 0.49, 1.49, 1.49);
  EXPECT_EQ(ClipRectSampleMask4(rect, {0, 0}, false), 15);
  EXPECT_EQ(ClipRectSampleMask4(rect, {0, 0}, true), 8);
}

TEST(ClipCoverage, InvalidGeometryCannotBecomeAnEmptyDifferenceMask) {
  const auto infinity = std::numeric_limits<Scalar>::infinity();
  const auto nan = std::numeric_limits<Scalar>::quiet_NaN();
  EXPECT_FALSE(ClipRectSampleMask4(Rect::MakeLTRB(0, 0, infinity, 1), {0, 0}));
  EXPECT_FALSE(ComputeRectClipFringe4(Rect::MakeLTRB(0, nan, 1, 1)));
  const auto rect = Rect::MakeLTRB(0, 0, 1, 1);
  EXPECT_FALSE(ClipRectSampleMask4(rect, {0.5, 0}));
  EXPECT_FALSE(ClipRectSampleMask4(rect, {nan, 0}));
  auto locations = kClipSampleLocations4;
  locations[0] = {1, 0};
  EXPECT_FALSE(ClipRectSampleMask4(rect, {0, 0}, true, locations));
  EXPECT_FALSE(ComputeRectClipFringe4(rect, locations));
}

TEST(ClipCoverage, FringeIsADisjointPartitionWithExactFullSampleInterior) {
  for (Scalar left : {-2.875f, -1.5f, -0.125f, 0.0f, 0.375f, 1.875f}) {
    for (Scalar top : {-1.875f, -0.5f, 0.0f, 0.625f}) {
      for (Scalar width : {0.01f, 0.5f, 1.0f, 3.375f}) {
        for (Scalar height : {0.125f, 0.75f, 1.0f, 4.0f}) {
          const auto rect = Rect::MakeXYWH(left, top, width, height);
          const auto fringe = ComputeRectClipFringe4(rect);
          ASSERT_TRUE(fringe);
          for (int64_t y = -4; y < 8; y++) {
            for (int64_t x = -4; x < 8; x++) {
              const IPoint pixel{x, y};
              const bool interior =
                  fringe->full_coverage_pixels.Contains(pixel);
              uint32_t members = interior ? 1u : 0u;
              for (const auto& strip : fringe->fringe_pixels) {
                members += strip.Contains(pixel) ? 1u : 0u;
              }
              EXPECT_EQ(members,
                        fringe->outer_pixels.Contains(pixel) ? 1u : 0u);
              const auto mask = ClipRectSampleMask4(rect, Point(pixel));
              ASSERT_TRUE(mask);
              EXPECT_EQ(interior, mask.value() == kClipSampleMask4Full);
              if (!fringe->outer_pixels.Contains(pixel)) {
                EXPECT_EQ(mask.value(), 0);
              }
            }
          }
        }
      }
    }
  }
}

TEST(ClipCoverage, FullInteriorExcludesSamplesExactlyOnRightOrBottom) {
  const auto fringe =
      ComputeRectClipFringe4(Rect::MakeLTRB(0, 0, 1.875, 1.875));
  ASSERT_TRUE(fringe);
  EXPECT_TRUE(fringe->full_coverage_pixels.Contains(IPoint{0, 0}));
  EXPECT_FALSE(fringe->full_coverage_pixels.Contains(IPoint{1, 1}));
  EXPECT_NE(ClipRectSampleMask4(Rect::MakeLTRB(0, 0, 1.875, 1.875), {1, 1}),
            15);
}

TEST(ClipCoverage, OnlyProvenUniformSrcOverMayUseResolvedCoverage) {
  ClipCoverageDrawClassifier proven;
  EXPECT_EQ(proven.GetStrategy(), ClipCoverageStrategy::kScissorOnly);
  proven.RecordDraw(true, BlendMode::kSrcOver, true);
  EXPECT_EQ(proven.GetStrategy(), ClipCoverageStrategy::kPerDraw);
  proven.RecordDraw(true, BlendMode::kSrcOver, true);
  EXPECT_EQ(proven.GetStrategy(), ClipCoverageStrategy::kFringeLayer);
  EXPECT_EQ(proven.GetFringeDrawCount(), 2u);
  proven.RecordDraw(true, BlendMode::kSrcOver);
  EXPECT_EQ(proven.GetStrategy(), ClipCoverageStrategy::kFringeIsland);
  for (auto mode : {BlendMode::kClear, BlendMode::kSrc, BlendMode::kDstIn,
                    BlendMode::kScreen}) {
    ClipCoverageDrawClassifier destructive;
    destructive.RecordDraw(true, mode, true);
    EXPECT_EQ(destructive.GetStrategy(), ClipCoverageStrategy::kFringeIsland);
  }
}

TEST(ClipCoverage, InteriorDestinationReadStillFlushesPrecedingFringeWork) {
  ClipCoverageDrawClassifier classifier;
  classifier.RecordDraw(true, BlendMode::kSrcOver, true);
  classifier.RecordDraw(false, BlendMode::kSrcOver, true, true);
  EXPECT_EQ(classifier.GetStrategy(),
            ClipCoverageStrategy::kDestinationReadBarrier);
  // Once a parent capture is already materialized, it is an ordinary source.
  ClipCoverageDrawClassifier captured;
  captured.RecordDraw(true, BlendMode::kSrcOver, true, false);
  EXPECT_EQ(captured.GetStrategy(), ClipCoverageStrategy::kPerDraw);
}

}  // namespace testing
}  // namespace impeller
