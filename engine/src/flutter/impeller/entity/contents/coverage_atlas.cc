// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/coverage_atlas.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace impeller {
namespace {

int64_t AlignDown(int64_t value) {
  auto remainder = value % kCoverageTileAlignment;
  return value -
         (remainder < 0 ? remainder + kCoverageTileAlignment : remainder);
}

int64_t AlignUp(int64_t value) {
  return AlignDown(value + kCoverageTileAlignment - 1);
}

bool IsFinite(const Rect& rect) {
  for (auto value : rect.GetLTRB()) {
    if (!std::isfinite(value)) {
      return false;
    }
  }
  return true;
}

}  // namespace

CoverageTilePlan::CoverageTilePlan(IRect content, IRect raster, ISize capacity)
    : content_(content),
      raster_(raster),
      capacity_(capacity),
      next_(raster.GetOrigin()) {}

std::optional<CoverageAtlasTile> CoverageTilePlan::Next() {
  if (raster_.IsEmpty() || next_.y >= raster_.GetBottom()) {
    return std::nullopt;
  }
  auto tile = IRect::MakeLTRB(
      next_.x, next_.y, std::min(next_.x + capacity_.width, raster_.GetRight()),
      std::min(next_.y + capacity_.height, raster_.GetBottom()));
  next_.x = tile.GetRight();
  if (next_.x >= raster_.GetRight()) {
    next_.x = raster_.GetLeft();
    next_.y = tile.GetBottom();
  }
  auto content = content_.Intersection(tile);
  return CoverageAtlasTile{tile, content.value_or(IRect{})};
}

std::optional<CoverageTilePlan> CoverageAtlas::PlanTiles(Rect coverage,
                                                         IRect destination,
                                                         ISize tile_capacity) {
  if (!IsFinite(coverage) || destination.IsEmpty() ||
      tile_capacity.width < kCoverageTileAlignment ||
      tile_capacity.height < kCoverageTileAlignment ||
      tile_capacity.width > std::numeric_limits<int32_t>::max() ||
      tile_capacity.height > std::numeric_limits<int32_t>::max()) {
    return std::nullopt;
  }
  // RenderPass scissors are signed 32-bit. This also leaves ample headroom for
  // alignment and iterator arithmetic before conversion to physical scissors.
  for (auto value : destination.GetLTRB()) {
    if (value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) {
      return std::nullopt;
    }
  }
  tile_capacity.width = AlignDown(tile_capacity.width);
  tile_capacity.height = AlignDown(tile_capacity.height);
  // Clamp before converting floats to integers. Geometry outside the output
  // cannot make the plan allocate more storage or overflow its coordinates.
  auto clipped = coverage.Intersection(Rect::Make(destination));
  if (!clipped.has_value()) {
    return CoverageTilePlan({}, {}, tile_capacity);
  }
  auto content = IRect::RoundOut(clipped.value()).Intersection(destination);
  if (!content.has_value()) {
    return CoverageTilePlan({}, {}, tile_capacity);
  }
  auto raster = IRect::MakeLTRB(
      AlignDown(content->GetLeft()), AlignDown(content->GetTop()),
      AlignUp(content->GetRight()), AlignUp(content->GetBottom()));
  return CoverageTilePlan(content.value(), raster, tile_capacity);
}

}  // namespace impeller
