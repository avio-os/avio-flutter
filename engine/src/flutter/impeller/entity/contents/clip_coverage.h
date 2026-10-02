// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_CLIP_COVERAGE_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_CLIP_COVERAGE_H_

#include <array>
#include <cstdint>
#include <optional>

#include "impeller/entity/clip_operation.h"
#include "impeller/geometry/color.h"
#include "impeller/geometry/point.h"
#include "impeller/geometry/rect.h"

namespace impeller {

/// One bit for each covered sample, never resolved alpha. AND and AND-NOT
/// preserve the correlation of nested clips; multiplying resolved alphas does
/// not. Textures carrying these bits must use nearest, unfiltered integer-grid
/// lookup. An unresolved binary 4x R8 mask is an equivalent representation.
using ClipSampleMask4 = uint8_t;
constexpr ClipSampleMask4 kClipSampleMask4Full = 0x0f;
using ClipSampleLocations4 = std::array<Point, 4>;

/// Vulkan's standard four-sample positions in top-left physical pixel space.
/// A backend must establish this pattern (or supply its actual pattern) before
/// claiming parity with its rasterized masks. This is not a uniform 2x2 grid.
constexpr ClipSampleLocations4 kClipSampleLocations4 = {
    Point{0.375f, 0.125f}, Point{0.875f, 0.375f}, Point{0.125f, 0.625f},
    Point{0.625f, 0.875f}};

ClipSampleMask4 CombineClipSampleMask4(ClipSampleMask4 parent,
                                       ClipSampleMask4 shape,
                                       ClipOperation operation);

/// Resolve only after intersecting every clip and the draw's own sample mask.
Scalar ResolveClipSampleMask4(ClipSampleMask4 mask);

/// Rectangles use half-open physical bounds. Non-AA rectangles preserve the
/// existing rounded-scissor policy; AA edges are never approximately rounded.
/// Nullopt reports invalid/non-finite bounds, sample locations, or a pixel
/// origin which is not on the integral physical grid. It must not be converted
/// to an empty difference mask (which would fail open).
std::optional<ClipSampleMask4> ClipRectSampleMask4(
    const Rect& rect,
    Point pixel_origin,
    bool is_aa = true,
    const ClipSampleLocations4& locations = kClipSampleLocations4);

bool CanUseRectClipScissor4(const Rect& rect, ClipOperation operation);

struct RectClipFringe4 {
  IRect outer_pixels;
  IRect full_coverage_pixels;
  /// At most four disjoint strips. Empty entries carry no work. Together they
  /// cover outer_pixels minus full_coverage_pixels; they may conservatively
  /// include pixels with zero coverage, which the sample mask removes.
  std::array<IRect, 4> fringe_pixels;
};

std::optional<RectClipFringe4> ComputeRectClipFringe4(
    const Rect& rect,
    const ClipSampleLocations4& locations = kClipSampleLocations4);

enum class ClipCoverageStrategy {
  kScissorOnly,
  kPerDraw,
  kFringeLayer,
  kFringeIsland,
  kDestinationReadBarrier,
};

/// Conservative classifier for one clip segment. It deliberately requires a
/// proof of uniform source samples AND complete draw geometry on the fringe.
/// Unknown/own-edge coverage requires a 4x colour island. A live destination
/// read must first flush preceding fringe work and capture the actual parent;
/// merely having a BackdropFilter type does not prove a captured source exists.
class ClipCoverageDrawClassifier {
 public:
  void RecordDraw(bool touches_fringe,
                  BlendMode blend_mode,
                  bool source_has_uniform_samples = false,
                  bool reads_live_destination = false);

  ClipCoverageStrategy GetStrategy() const;

  uint32_t GetFringeDrawCount() const { return fringe_draw_count_; }

 private:
  uint32_t fringe_draw_count_ = 0;
  bool all_src_over_ = true;
  bool all_uniform_samples_ = true;
  bool reads_live_destination_ = false;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_CLIP_COVERAGE_H_
