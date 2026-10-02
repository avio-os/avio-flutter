// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/clip_coverage.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace impeller {
namespace {

bool ValidLocations(const ClipSampleLocations4& locations) {
  return std::all_of(locations.begin(), locations.end(), [](Point p) {
    return p.IsFinite() && p.x >= 0 && p.x < 1 && p.y >= 0 && p.y < 1;
  });
}

bool Integral(Scalar value) {
  return std::isfinite(value) && std::floor(value) == value;
}

int64_t CeilCoordinate(Scalar value) {
  return saturated::Cast<Scalar, int64_t>(std::ceil(value));
}

}  // namespace

ClipSampleMask4 CombineClipSampleMask4(ClipSampleMask4 parent,
                                       ClipSampleMask4 shape,
                                       ClipOperation operation) {
  parent &= kClipSampleMask4Full;
  shape &= kClipSampleMask4Full;
  switch (operation) {
    case ClipOperation::kIntersect:
      return parent & shape;
    case ClipOperation::kDifference:
      return parent & ~shape;
  }
  return 0;
}

Scalar ResolveClipSampleMask4(ClipSampleMask4 mask) {
  mask &= kClipSampleMask4Full;
  uint32_t count = 0;
  for (size_t i = 0; i < 4; i++) {
    count += (mask >> i) & 1;
  }
  return static_cast<Scalar>(count) * 0.25f;
}

std::optional<ClipSampleMask4> ClipRectSampleMask4(
    const Rect& rect,
    Point pixel_origin,
    bool is_aa,
    const ClipSampleLocations4& locations) {
  if (!rect.IsFinite() || !Integral(pixel_origin.x) ||
      !Integral(pixel_origin.y) || !ValidLocations(locations)) {
    return std::nullopt;
  }
  const Rect bounds = is_aa ? rect : Rect::Round(rect);
  ClipSampleMask4 mask = 0;
  for (size_t i = 0; i < locations.size(); i++) {
    if (bounds.Contains(pixel_origin + locations[i])) {
      mask |= 1u << i;
    }
  }
  return mask;
}

bool CanUseRectClipScissor4(const Rect& rect, ClipOperation operation) {
  return operation == ClipOperation::kIntersect && rect.IsFinite() &&
         Integral(rect.GetLeft()) && Integral(rect.GetTop()) &&
         Integral(rect.GetRight()) && Integral(rect.GetBottom());
}

std::optional<RectClipFringe4> ComputeRectClipFringe4(
    const Rect& rect,
    const ClipSampleLocations4& locations) {
  if (!rect.IsFinite() || !ValidLocations(locations)) {
    return std::nullopt;
  }
  if (rect.IsEmpty()) {
    return RectClipFringe4{};
  }
  Point minimum = locations.front();
  Point maximum = minimum;
  for (Point point : locations) {
    minimum.x = std::min(minimum.x, point.x);
    minimum.y = std::min(minimum.y, point.y);
    maximum.x = std::max(maximum.x, point.x);
    maximum.y = std::max(maximum.y, point.y);
  }
  const IRect outer = IRect::RoundOut(rect);
  // For the exclusive right/bottom bound, ceil(bound - largest_sample) is
  // exclusive too. Using floor would admit a sample on an excluded edge.
  const IRect inner =
      IRect::MakeLTRB(CeilCoordinate(rect.GetLeft() - minimum.x),
                      CeilCoordinate(rect.GetTop() - minimum.y),
                      CeilCoordinate(rect.GetRight() - maximum.x),
                      CeilCoordinate(rect.GetBottom() - maximum.y));
  if (inner.IsEmpty()) {
    return RectClipFringe4{.outer_pixels = outer,
                           .full_coverage_pixels = {},
                           .fringe_pixels = {outer, {}, {}, {}}};
  }
  return RectClipFringe4{
      .outer_pixels = outer,
      .full_coverage_pixels = inner,
      .fringe_pixels = {
          IRect::MakeLTRB(outer.GetLeft(), outer.GetTop(), outer.GetRight(),
                          inner.GetTop()),
          IRect::MakeLTRB(outer.GetLeft(), inner.GetBottom(), outer.GetRight(),
                          outer.GetBottom()),
          IRect::MakeLTRB(outer.GetLeft(), inner.GetTop(), inner.GetLeft(),
                          inner.GetBottom()),
          IRect::MakeLTRB(inner.GetRight(), inner.GetTop(), outer.GetRight(),
                          inner.GetBottom()),
      }};
}

void ClipCoverageDrawClassifier::RecordDraw(bool touches_fringe,
                                            BlendMode blend_mode,
                                            bool source_has_uniform_samples,
                                            bool reads_live_destination) {
  // A destination read inside the clip can depend on preceding fringe output
  // even when the read's own bounds lie strictly in the clip's interior.
  reads_live_destination_ |= reads_live_destination;
  if (!touches_fringe) {
    return;
  }
  if (fringe_draw_count_ < std::numeric_limits<uint32_t>::max()) {
    fringe_draw_count_++;
  }
  all_src_over_ &= blend_mode == BlendMode::kSrcOver;
  all_uniform_samples_ &= source_has_uniform_samples;
}

ClipCoverageStrategy ClipCoverageDrawClassifier::GetStrategy() const {
  if (reads_live_destination_) {
    return ClipCoverageStrategy::kDestinationReadBarrier;
  }
  if (fringe_draw_count_ == 0) {
    return ClipCoverageStrategy::kScissorOnly;
  }
  if (!all_src_over_ || !all_uniform_samples_) {
    return ClipCoverageStrategy::kFringeIsland;
  }
  return fringe_draw_count_ == 1 ? ClipCoverageStrategy::kPerDraw
                                 : ClipCoverageStrategy::kFringeLayer;
}

}  // namespace impeller
