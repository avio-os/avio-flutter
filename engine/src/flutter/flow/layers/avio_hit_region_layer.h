// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_FLOW_LAYERS_AVIO_HIT_REGION_LAYER_H_
#define FLUTTER_FLOW_LAYERS_AVIO_HIT_REGION_LAYER_H_

#include "flutter/flow/avio_hit_region.h"
#include "flutter/flow/layers/container_layer.h"

namespace flutter {

// A retained input claim authored in the same layer tree as the pixels it
// describes. Its children paint unchanged (translated by |offset|); the layer
// adds no paint bounds, never forces a saveLayer and never damages pixels.
//
// The claim is collected only by the frame-facts preroll, from the same
// transform, clip and opacity state that positions the children, so an
// ancestor move, scroll or clip moves the claim with the pixels in the same
// frame. A disabled layer, or one under zero outstanding opacity, claims
// nothing. A malformed rect, a non-axis-aligned transform or overflow past
// kMaxAvioHitRegionsPerFrame invalidates the whole set (fail closed).
class AvioHitRegionLayer final : public ContainerLayer {
 public:
  AvioHitRegionLayer(DlRect rect,
                     bool enabled,
                     AvioHitRegionKind kind,
                     DlPoint offset);

  const DlRect& rect() const { return rect_; }
  bool enabled() const { return enabled_; }
  AvioHitRegionKind kind() const { return kind_; }

  void Diff(DiffContext* context, const Layer* old_layer) override;
  void Preroll(PrerollContext* context) override;
  void Paint(PaintContext& context) const override;
  const AvioHitRegionLayer* as_avio_hit_region_layer() const override {
    return this;
  }

 private:
  void Collect(PrerollContext* context) const;

  DlRect rect_;
  bool enabled_;
  AvioHitRegionKind kind_;
  DlPoint offset_;
  bool malformed_;
};

}  // namespace flutter

#endif  // FLUTTER_FLOW_LAYERS_AVIO_HIT_REGION_LAYER_H_
