// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_COVERAGE_ATLAS_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_COVERAGE_ATLAS_H_

#include <optional>

#include "impeller/geometry/rect.h"
#include "impeller/geometry/size.h"

namespace impeller {

// The parent and island raster grids have identical sample and derivative
// phases. Eight also preserves the derivative quads used by image sampling.
inline constexpr int64_t kCoverageTileAlignment = 8;

struct CoverageAtlasTile {
  // Physical parent coordinates, aligned outwards. The island has this size;
  // pixels outside content_rect must not be copied back into the parent.
  IRect raster_rect;
  IRect content_rect;
};

// Streams a large logical region through fixed storage. Planning never creates
// a vector proportional to the path bounds or increases the physical atlas.
class CoverageTilePlan {
 public:
  std::optional<CoverageAtlasTile> Next();

 private:
  CoverageTilePlan(IRect content, IRect raster, ISize capacity);

  IRect content_;
  IRect raster_;
  ISize capacity_;
  IPoint next_;

  friend class CoverageAtlas;
};

class CoverageAtlas {
 public:
  // coverage is already expanded for any source/filter support. destination
  // is the writable parent extent. A zero-size/invalid plan is rejected;
  // disjoint coverage produces a valid empty iterator. Capacity is the content
  // extent offered by the fixed region allocator, excluding its gutter.
  static std::optional<CoverageTilePlan> PlanTiles(Rect coverage,
                                                   IRect destination,
                                                   ISize tile_capacity);
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_COVERAGE_ATLAS_H_
