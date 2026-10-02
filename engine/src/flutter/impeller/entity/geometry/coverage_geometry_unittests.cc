// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/geometry/coverage_geometry.h"

#include <limits>

#include "gtest/gtest.h"

namespace impeller {
namespace testing {

TEST(CoverageGeometryTest, QuadMatchesRectangleSampleIdentity) {
  for (Scalar x : {-1.25f, 0.0f, 0.125f, 0.375f, 0.5f, 1.25f}) {
    auto bounds = Rect::MakeLTRB(x, -0.125, x + 0.75, 1.625);
    auto quad = CoverageConvexQuad4::Make(bounds.GetPoints());
    ASSERT_TRUE(quad.has_value());
    for (int row = -2; row <= 2; row++) {
      for (int column = -2; column <= 2; column++) {
        auto pixel =
            Point{static_cast<Scalar>(column), static_cast<Scalar>(row)};
        ASSERT_EQ(quad->GetSampleMask(pixel),
                  ClipRectSampleMask4(bounds, pixel));
      }
    }
  }
}

TEST(CoverageGeometryTest, ReflectedAndObliqueQuadRetainFourBits) {
  auto reflected = CoverageConvexQuad4::Make(
      {Point{0.5, 0}, Point{0, 0}, Point{0.5, 1}, Point{0, 1}});
  ASSERT_TRUE(reflected.has_value());
  ASSERT_EQ(reflected->GetSampleMask({0, 0}), 0x5);
  auto diamond = CoverageConvexQuad4::Make(
      {Point{0.5, 0}, Point{1, 0.5}, Point{0, 0.5}, Point{0.5, 1}});
  ASSERT_TRUE(diamond.has_value());
  ASSERT_EQ(diamond->GetSampleMask({0, 0}), 0x5);
}

TEST(CoverageGeometryTest, AdjacentEdgesPartitionBoundarySamples) {
  auto left =
      CoverageConvexQuad4::Make(Rect::MakeLTRB(0, 0, 0.375, 1).GetPoints());
  auto right =
      CoverageConvexQuad4::Make(Rect::MakeLTRB(0.375, 0, 1, 1).GetPoints());
  ASSERT_TRUE(left.has_value());
  ASSERT_TRUE(right.has_value());
  auto left_bits = left->GetSampleMask({0, 0});
  auto right_bits = right->GetSampleMask({0, 0});
  ASSERT_TRUE(left_bits.has_value());
  ASSERT_TRUE(right_bits.has_value());
  ASSERT_EQ(left_bits.value() & right_bits.value(), 0);
  ASSERT_EQ(left_bits.value() | right_bits.value(), kClipSampleMask4Full);
}

TEST(CoverageGeometryTest, TiledViewportShiftPreservesLogicalSampleGrid) {
  Quad points = Rect::MakeLTRB(0.375, 0.125, 2.625, 1.875).GetPoints();
  auto original = CoverageConvexQuad4::Make(points);
  ASSERT_TRUE(original.has_value());
  for (Point tile_origin : {Point{-248, -16}, Point{248, 504}}) {
    Quad translated_points;
    for (size_t index = 0; index < points.size(); index++) {
      translated_points[index] = points[index] + tile_origin;
    }
    auto translated = CoverageConvexQuad4::Make(translated_points);
    ASSERT_TRUE(translated.has_value());
    ASSERT_EQ(translated->GetBounds(),
              original->GetBounds().Shift(tile_origin));
    for (int row = -1; row <= 2; row++) {
      for (int column = -1; column <= 3; column++) {
        Point pixel{static_cast<Scalar>(column), static_cast<Scalar>(row)};
        ASSERT_EQ(original->GetSampleMask(pixel),
                  translated->GetSampleMask(pixel + tile_origin));
      }
    }
  }
}

TEST(CoverageGeometryTest, RejectsInvalidShapeAndPixelGrid) {
  ASSERT_FALSE(CoverageConvexQuad4::Make(
      {Point{0, 0}, Point{1, 0}, Point{0, 1}, Point{0.25, 0.25}}));
  ASSERT_FALSE(
      CoverageConvexQuad4::Make(Rect::MakeLTRB(0, 0, 0, 1).GetPoints()));
  auto quad = CoverageConvexQuad4::Make(Rect::MakeLTRB(0, 0, 1, 1).GetPoints());
  ASSERT_TRUE(quad.has_value());
  ASSERT_FALSE(quad->GetSampleMask({0.5, 0}));
  auto invalid_locations = kClipSampleLocations4;
  invalid_locations[0].x = std::numeric_limits<Scalar>::quiet_NaN();
  ASSERT_FALSE(quad->GetSampleMask({0, 0}, invalid_locations));
}

TEST(CoverageGeometryTest, RasterKeyUsesExactTransformTileAndSamplePhase) {
  auto bounds = IRect::MakeLTRB(0, 0, 8, 8);
  Matrix transform;
  auto key = CoverageRasterKey4::Make(transform, bounds);
  ASSERT_TRUE(key.has_value());
  ASSERT_EQ(key, CoverageRasterKey4::Make(transform, bounds));
  transform.m[12] = 0.000001f;
  ASSERT_NE(key, CoverageRasterKey4::Make(transform, bounds));
  ASSERT_NE(key, CoverageRasterKey4::Make(Matrix{}, bounds.Shift(8, 0)));
  auto phase = kClipSampleLocations4;
  phase[0].x += 0.125f;
  ASSERT_NE(key, CoverageRasterKey4::Make(Matrix{}, bounds, phase));
  transform.m[3] = std::numeric_limits<Scalar>::infinity();
  ASSERT_FALSE(CoverageRasterKey4::Make(transform, bounds));
}

}  // namespace testing
}  // namespace impeller
