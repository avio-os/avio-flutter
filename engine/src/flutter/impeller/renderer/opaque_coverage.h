// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_OPAQUE_COVERAGE_H_
#define FLUTTER_IMPELLER_RENDERER_OPAQUE_COVERAGE_H_

#include <limits>
#include <optional>

#include "impeller/core/formats.h"
#include "impeller/geometry/color.h"
#include "impeller/geometry/rect.h"

namespace impeller {

// Evidence about pixels actually cleared or overwritten in one owning target.
// A Load never establishes evidence from a format, label, or prior tenant.
class OpaqueCoverageState {
 public:
  void RecordClear(LoadAction action, Scalar alpha, Rect bounds) {
    opaque_ = action == LoadAction::kClear && alpha == 1.f &&
                      bounds.IsFinite() && !bounds.IsEmpty()
                  ? std::optional<Rect>(bounds)
                  : std::nullopt;
  }

  void RecordDraw(BlendMode blend,
                  std::optional<Rect> full_opaque_overwrite = std::nullopt,
                  bool source_is_opaque = false) {
    if (blend != BlendMode::kSrcOver && blend != BlendMode::kSrc) {
      opaque_.reset();
      return;
    }
    if (full_opaque_overwrite && full_opaque_overwrite->IsFinite() &&
        !full_opaque_overwrite->IsEmpty()) {
      // Retaining either certificate is conservative; an arbitrary union
      // could introduce holes between disjoint opaque rectangles.
      if (!opaque_ || full_opaque_overwrite->Contains(*opaque_)) {
        opaque_ = full_opaque_overwrite;
      }
    } else if (blend == BlendMode::kSrc && !source_is_opaque) {
      opaque_.reset();
    }
  }

  const std::optional<Rect>& GetOpaqueRect() const { return opaque_; }

 private:
  std::optional<Rect> opaque_;
};

// Propagates a certified opaque input through an actual axis-aligned sampling
// map. The radius includes every explicit tap and its linear-filter footprint.
// Output bounds are whole pixels whose complete footprint remains inside the
// input certificate; decal/padding pixels never acquire opaque evidence.
inline std::optional<Rect> MapOpaqueSamplingFootprint(
    Rect opaque_input,
    ISize input_size,
    const std::array<Point, 4>& input_uvs,
    IRect output_content,
    Vector2 sample_radius) {
  if (!opaque_input.IsFinite() || opaque_input.IsEmpty() ||
      input_size.IsEmpty() || output_content.IsEmpty() ||
      output_content.GetWidth() > std::numeric_limits<int32_t>::max() ||
      output_content.GetHeight() > std::numeric_limits<int32_t>::max() ||
      !sample_radius.IsFinite() || sample_radius.x < 0 || sample_radius.y < 0) {
    return std::nullopt;
  }
  const auto p = input_uvs[0] * Vector2(input_size);
  const auto x = (input_uvs[1] - input_uvs[0]) * Vector2(input_size);
  const auto y = (input_uvs[2] - input_uvs[0]) * Vector2(input_size);
  if (!p.IsFinite() || !x.IsFinite() || !y.IsFinite() || x.y != 0 || y.x != 0 ||
      x.x <= 0 || y.y <= 0 ||
      input_uvs[3] != input_uvs[1] + input_uvs[2] - input_uvs[0]) {
    return std::nullopt;
  }
  const auto safe = opaque_input.Expand(-sample_radius);
  if (safe.IsEmpty()) {
    return std::nullopt;
  }
  const auto scale = Vector2(output_content.GetSize()) / Vector2(x.x, y.y);
  const Rect mapped = Rect::MakeLTRB(
      (safe.GetLeft() - p.x) * scale.x, (safe.GetTop() - p.y) * scale.y,
      (safe.GetRight() - p.x) * scale.x, (safe.GetBottom() - p.y) * scale.y);
  if (!mapped.IsFinite()) {
    return std::nullopt;
  }
  const auto clipped =
      mapped.Intersection(Rect::MakeSize(output_content.GetSize()));
  const auto pixels =
      clipped ? std::optional<IRect>(IRect::RoundIn(*clipped)) : std::nullopt;
  return pixels && !pixels->IsEmpty()
             ? std::optional<Rect>(
                   Rect::Make(*pixels).Shift(Point(output_content.GetOrigin())))
             : std::nullopt;
}

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_OPAQUE_COVERAGE_H_
