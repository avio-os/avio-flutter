// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <array>
#include "gtest/gtest.h"
#include "impeller/golden_tests/golden_edge_comparison.h"

namespace impeller::testing {
TEST(GoldenEdgeComparisonTest,
     ActualAliasedMaskAndInteriorEqualityAreRequired) {
  const std::array<uint8_t, 8> baseline = {20, 20, 20, 255, 50, 50, 50, 255};
  auto coverage = baseline, aliased = baseline;
  aliased[0] = 0;
  coverage[0]++;
  std::array<uint8_t, 8> mask;
  auto result = CompareGoldenEdges(baseline, coverage, aliased, mask);
  EXPECT_TRUE(result.Passes(false));
  EXPECT_FALSE(result.Passes(true));
  EXPECT_EQ(result.edge_pixels, 1u);
  EXPECT_EQ(mask[0], 255);
  EXPECT_EQ(mask[4], 0);
  coverage[4]++;
  result = CompareGoldenEdges(baseline, coverage, aliased, mask);
  EXPECT_FALSE(result.Passes(false));
  EXPECT_EQ(result.max_interior_delta, 1u);
  EXPECT_EQ(result.changed_interior_pixels, 1u);
}
TEST(GoldenEdgeComparisonTest, EdgeChangeAboveOneIsAFailureNotARecordedPass) {
  const std::array<uint8_t, 4> baseline = {64, 64, 64, 255};
  auto aliased = baseline, coverage = baseline;
  aliased[3] = 0;
  coverage[0] += 2;
  std::array<uint8_t, 4> mask;
  const auto result = CompareGoldenEdges(baseline, coverage, aliased, mask);
  EXPECT_FALSE(result.Passes(false));
  EXPECT_EQ(result.max_edge_delta, 2u);
}
TEST(GoldenEdgeComparisonTest, MissingAndMismatchedReadbacksNeverPass) {
  std::array<uint8_t, 4> pixels = {};
  EXPECT_FALSE(CompareGoldenEdges({}, {}, {}, {}).Passes(false));
  EXPECT_FALSE(CompareGoldenEdges(pixels, pixels, {}, pixels).Passes(false));
}
}  // namespace impeller::testing
