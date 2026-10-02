// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/coverage_atlas.h"

#include <limits>

#include "gtest/gtest.h"

namespace impeller {
namespace testing {

TEST(CoverageAtlasTest, StreamsLargePathThroughFixedTiles) {
  auto plan = CoverageAtlas::PlanTiles(
      Rect::MakeLTRB(-10000, -10000, 10000, 10000),
      IRect::MakeLTRB(0, 0, 1921, 1081), ISize{248, 248});
  ASSERT_TRUE(plan.has_value());
  size_t count = 0;
  int64_t total_area = 0;
  while (auto tile = plan->Next()) {
    ASSERT_LE(tile->raster_rect.GetWidth(), 248);
    ASSERT_LE(tile->raster_rect.GetHeight(), 248);
    ASSERT_EQ(tile->raster_rect.GetLeft() % kCoverageTileAlignment, 0);
    ASSERT_EQ(tile->raster_rect.GetTop() % kCoverageTileAlignment, 0);
    ASSERT_EQ(tile->raster_rect.GetWidth() % kCoverageTileAlignment, 0);
    ASSERT_EQ(tile->raster_rect.GetHeight() % kCoverageTileAlignment, 0);
    ASSERT_TRUE(tile->raster_rect.Contains(tile->content_rect));
    total_area +=
        tile->content_rect.GetWidth() * tile->content_rect.GetHeight();
    count++;
  }
  ASSERT_EQ(count, 40u);
  ASSERT_EQ(total_area, 1921 * 1081);
}

TEST(CoverageAtlasTest, PreservesGlobalPhaseAtNegativeAndFractionalOrigin) {
  auto plan =
      CoverageAtlas::PlanTiles(Rect::MakeLTRB(-13.5, -1.25, 11.25, 8.5),
                               IRect::MakeLTRB(-15, -3, 17, 13), ISize{19, 19});
  ASSERT_TRUE(plan.has_value());
  auto first = plan->Next();
  ASSERT_TRUE(first.has_value());
  ASSERT_EQ(first->raster_rect, IRect::MakeLTRB(-16, -8, 0, 8));
  ASSERT_EQ(first->content_rect, IRect::MakeLTRB(-14, -2, 0, 8));
  int64_t total_area =
      first->content_rect.GetWidth() * first->content_rect.GetHeight();
  size_t count = 1;
  while (auto tile = plan->Next()) {
    ASSERT_LE(tile->raster_rect.GetWidth(), 16);
    ASSERT_LE(tile->raster_rect.GetHeight(), 16);
    total_area +=
        tile->content_rect.GetWidth() * tile->content_rect.GetHeight();
    count++;
  }
  ASSERT_EQ(count, 4u);
  ASSERT_EQ(total_area, 26 * 11);
}

TEST(CoverageAtlasTest, EmptyCoverageDoesNotAcquireStorage) {
  auto plan =
      CoverageAtlas::PlanTiles(Rect::MakeLTRB(100, 100, 200, 200),
                               IRect::MakeLTRB(0, 0, 16, 16), ISize{16, 16});
  ASSERT_TRUE(plan.has_value());
  ASSERT_FALSE(plan->Next().has_value());
  ASSERT_FALSE(plan->Next().has_value());
}

TEST(CoverageAtlasTest, RejectsNonFiniteCoverageAndUnusableCapacity) {
  auto output = IRect::MakeLTRB(0, 0, 100, 100);
  ASSERT_FALSE(CoverageAtlas::PlanTiles(
      Rect::MakeLTRB(0, 0, std::numeric_limits<Scalar>::infinity(), 10), output,
      ISize{16, 16}));
  ASSERT_FALSE(CoverageAtlas::PlanTiles(Rect::MakeLTRB(0, 0, 10, 10), output,
                                        ISize{7, 16}));
  ASSERT_FALSE(
      CoverageAtlas::PlanTiles(Rect::MakeLTRB(0, 0, 10, 10), output,
                               ISize{std::numeric_limits<int64_t>::max(), 16}));
}

}  // namespace testing
}  // namespace impeller
