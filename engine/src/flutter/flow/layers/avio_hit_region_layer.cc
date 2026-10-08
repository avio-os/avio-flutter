// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/flow/layers/avio_hit_region_layer.h"

#include <cmath>

namespace flutter {

namespace {
// A 90-degree rotation computed in floating point leaves ~1e-7 in the zero
// matrix terms. Treat that as axis-aligned; the bounding box then differs from
// the exact quad by far less than a device pixel.
constexpr DlScalar kAxisAlignmentTolerance = 1e-5f;
}  // namespace

AvioHitRegionLayer::AvioHitRegionLayer(DlRect rect,
                                       bool enabled,
                                       AvioHitRegionKind kind,
                                       DlPoint offset)
    : rect_(rect),
      enabled_(enabled),
      kind_(kind),
      offset_(offset),
      malformed_(!rect.IsFinite() || rect.GetWidth() < 0 ||
                 rect.GetHeight() < 0 || !std::isfinite(offset.x) ||
                 !std::isfinite(offset.y) ||
                 (kind != AvioHitRegionKind::kClaim &&
                  kind != AvioHitRegionKind::kOutputCapture)) {
  set_subtree_has_avio_hit_region(true);
}

void AvioHitRegionLayer::Diff(DiffContext* context, const Layer* old_layer) {
  DiffContext::AutoSubtreeRestore subtree(context);
  const auto* previous =
      old_layer ? old_layer->as_avio_hit_region_layer() : nullptr;
  // A claim (rect, enabled, kind) is not pixels and never damages. A changed
  // offset still translates the child pixels and invalidates their old region.
  if (!context->IsSubtreeDirty() &&
      (!previous || previous->offset_ != offset_)) {
    if (previous) {
      context->MarkSubtreeDirty(context->GetOldLayerPaintRegion(previous));
    } else {
      context->MarkSubtreeDirty();
    }
  }
  context->PushTransform(DlMatrix::MakeTranslation(offset_));
  DiffChildren(context, previous);
  context->SetLayerPaintRegion(this, context->CurrentSubtreeRegion());
}

void AvioHitRegionLayer::Preroll(PrerollContext* context) {
  auto restore = context->state_stack.save();
  restore.translate(offset_.x, offset_.y);
  // Pre-order: an enclosing claim precedes the claims nested in its children.
  if (context->avio_hit_regions) {
    Collect(context);
  }
  // ContainerLayer::Preroll leaves the children's renderable state flags in
  // the context, so an ancestor opacity or filter is never forced into a
  // saveLayer by this layer.
  ContainerLayer::Preroll(context);
  set_paint_bounds(paint_bounds().Shift(offset_.x, offset_.y));
}

void AvioHitRegionLayer::Collect(PrerollContext* context) const {
  auto& regions = *context->avio_hit_regions;
  if (malformed_) {
    regions.Invalidate();
    return;
  }
  if (!enabled_) {
    return;
  }
  const auto& state = context->state_stack;
  const float opacity = state.outstanding_opacity();
  if (!std::isfinite(opacity) || opacity < 0 || opacity > 1) {
    regions.Invalidate();
    return;
  }
  // Nothing visible claims nothing, the same frame its pixels vanish.
  if (opacity == 0 || rect_.IsEmpty()) {
    return;
  }
  const DlMatrix& matrix = state.matrix();
  if (!matrix.IsFinite() || !matrix.IsAligned2D(kAxisAlignmentTolerance)) {
    regions.Invalidate();
    return;
  }
  const DlRect device = rect_.TransformAndClipBounds(matrix);
  if (!device.IsFinite()) {
    regions.Invalidate();
    return;
  }
  // The scene cull carries every ancestor clip over the whole frame, also
  // when this preroll's raster cull is a partial damage rect.
  const auto clipped = device.Intersection(state.device_scene_cull_rect());
  if (!clipped.has_value() || clipped->IsEmpty()) {
    return;
  }
  regions.Add({*clipped, kind_});
}

void AvioHitRegionLayer::Paint(PaintContext& context) const {
  auto restore = context.state_stack.save();
  restore.translate(offset_.x, offset_.y);
  PaintChildren(context);
}

}  // namespace flutter
