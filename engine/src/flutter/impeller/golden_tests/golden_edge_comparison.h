// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_GOLDEN_TESTS_GOLDEN_EDGE_COMPARISON_H_
#define FLUTTER_IMPELLER_GOLDEN_TESTS_GOLDEN_EDGE_COMPARISON_H_

#include <algorithm>
#include <cstdint>
#include <span>

namespace impeller::testing {

struct GoldenEdgeComparison {
  bool valid = false;
  uint64_t edge_pixels = 0, changed_interior_pixels = 0;
  uint32_t max_edge_delta = 0, max_interior_delta = 0;
  bool Passes(bool byte_identical_class) const {
    return valid && max_interior_delta == 0 &&
           max_edge_delta <= (byte_identical_class ? 0u : 1u);
  }
};

// The edge mask is defined by actual baseline-vs-aliased bytes, including
// alpha. It is never guessed from a bounding rectangle or candidate output.
inline GoldenEdgeComparison CompareGoldenEdges(
    std::span<const uint8_t> baseline,
    std::span<const uint8_t> coverage,
    std::span<const uint8_t> aliased,
    std::span<uint8_t> rgba_edge_mask) {
  GoldenEdgeComparison result;
  if (baseline.empty() || baseline.size() % 4 ||
      coverage.size() != baseline.size() || aliased.size() != baseline.size() ||
      rgba_edge_mask.size() != baseline.size())
    return result;
  result.valid = true;
  for (size_t pixel = 0; pixel < baseline.size(); pixel += 4) {
    bool edge = false;
    uint32_t delta = 0;
    for (size_t channel = 0; channel < 4; ++channel) {
      const auto index = pixel + channel;
      edge |= baseline[index] != aliased[index];
      const uint32_t b = baseline[index], c = coverage[index];
      delta = std::max(delta, b > c ? b - c : c - b);
    }
    rgba_edge_mask[pixel] = rgba_edge_mask[pixel + 1] =
        rgba_edge_mask[pixel + 2] = edge ? 255 : 0;
    rgba_edge_mask[pixel + 3] = 255;
    if (edge) {
      result.edge_pixels++;
      result.max_edge_delta = std::max(result.max_edge_delta, delta);
    } else {
      result.changed_interior_pixels += delta != 0;
      result.max_interior_delta = std::max(result.max_interior_delta, delta);
    }
  }
  return result;
}

}  // namespace impeller::testing
#endif  // FLUTTER_IMPELLER_GOLDEN_TESTS_GOLDEN_EDGE_COMPARISON_H_
