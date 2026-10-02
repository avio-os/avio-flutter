// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_GEOMETRY_COVERAGE_GEOMETRY_H_
#define FLUTTER_IMPELLER_ENTITY_GEOMETRY_COVERAGE_GEOMETRY_H_

#include <optional>

#include "impeller/entity/contents/clip_coverage.h"
#include "impeller/geometry/matrix.h"

namespace impeller {

// Complete physical raster state used by the path-mask cache. Matrix's normal
// approximate equality is inappropriate for subpixel sample-phase identity.
struct CoverageRasterKey4 {
  std::array<uint32_t, 16> transform_bits;
  std::array<uint32_t, 8> sample_location_bits;
  IRect raster_rect;
  bool operator==(const CoverageRasterKey4&) const = default;

  static std::optional<CoverageRasterKey4> Make(
      const Matrix& transform,
      IRect raster_rect,
      const ClipSampleLocations4& locations = kClipSampleLocations4);
};

// Analytic edges for a transformed image rectangle or a convex quadrilateral.
// Input uses Rect::GetPoints order (TL, TR, BL, BR), including reflected quads.
// Concave, degenerate and non-finite quads must use the path/island renderer.
class CoverageConvexQuad4 {
 public:
  static std::optional<CoverageConvexQuad4> Make(const Quad& quad);

  std::optional<ClipSampleMask4> GetSampleMask(
      Point pixel_origin,
      const ClipSampleLocations4& locations = kClipSampleLocations4) const;

  // Each column is (A,B,C,inclusive) for Ax+By+C >= 0. Equality is accepted
  // only on top/left edges, so adjacent image quads share no boundary samples.
  // This exact representation is shared with coverage_geometry.glsl.
  const Matrix& GetLineParameters() const { return lines_; }

  const Rect& GetBounds() const { return bounds_; }

  // Integer raster tile translation preserves sample phase, winding and
  // top/left equality ownership. No vertex reconstruction is involved.
  CoverageConvexQuad4 Translated(Vector2 offset) const;

 private:
  CoverageConvexQuad4(Matrix lines, Rect bounds)
      : lines_(lines), bounds_(bounds) {}
  Matrix lines_;
  Rect bounds_;
};

}  // namespace impeller
#endif  // FLUTTER_IMPELLER_ENTITY_GEOMETRY_COVERAGE_GEOMETRY_H_
