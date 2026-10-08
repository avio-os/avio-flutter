// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_FLOW_LAYERS_AVIO_FRAME_METADATA_LAYER_H_
#define FLUTTER_FLOW_LAYERS_AVIO_FRAME_METADATA_LAYER_H_

#include "flutter/flow/avio_frame_facts.h"
#include "flutter/flow/layers/container_layer.h"

namespace flutter {

// Root metadata does not apply opacity to its child pixels or create a
// saveLayer. LayerTree validates the complete root prefix before painting.
class AvioFrameMetadataLayer final : public ContainerLayer {
 public:
  AvioFrameMetadataLayer(AvioFrameFacts facts, DlPoint offset);
  const AvioFrameFacts& facts() const { return facts_; }
  const AvioFrameMetadataLayer* as_avio_frame_metadata_layer() const override {
    return this;
  }
  void Diff(DiffContext* context, const Layer* old_layer) override;
  void Preroll(PrerollContext* context) override;
  void Paint(PaintContext& context) const override;

 private:
  AvioFrameFacts facts_;
  DlPoint offset_;
};

// Only the sole-child root prefix (including ordinary root transforms and
// hit-region claims) may author these facts. Duplicates, siblings and
// effect/filter ancestors reject the scene. The collector is allocation-free
// and also checks retained layers.
AvioFrameFacts CollectAvioRootFrameFacts(const Layer& root);

}  // namespace flutter

#endif  // FLUTTER_FLOW_LAYERS_AVIO_FRAME_METADATA_LAYER_H_
