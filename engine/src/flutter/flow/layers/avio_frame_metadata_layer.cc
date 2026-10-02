// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/flow/layers/avio_frame_metadata_layer.h"

namespace flutter {

AvioFrameMetadataLayer::AvioFrameMetadataLayer(AvioFrameFacts facts,
                                               DlPoint offset)
    : facts_(std::move(facts)), offset_(offset) {
  facts_.invalid |= !std::isfinite(offset.x) || !std::isfinite(offset.y);
  set_subtree_has_avio_frame_metadata(true);
}

void AvioFrameMetadataLayer::Diff(DiffContext* context,
                                  const Layer* old_layer) {
  DiffContext::AutoSubtreeRestore subtree(context);
  const auto* previous =
      old_layer ? old_layer->as_avio_frame_metadata_layer() : nullptr;
  // Opacity and ground changes are producer metadata damage only. A changed
  // offset is still a pixel transform and must invalidate the old paint region.
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

void AvioFrameMetadataLayer::Preroll(PrerollContext* context) {
  auto restore = context->state_stack.save();
  restore.translate(offset_.x, offset_.y);
  ContainerLayer::Preroll(context);
  set_paint_bounds(paint_bounds().Shift(offset_.x, offset_.y));
}

void AvioFrameMetadataLayer::Paint(PaintContext& context) const {
  auto restore = context.state_stack.save();
  restore.translate(offset_.x, offset_.y);
  PaintChildren(context);
}

namespace {
void Collect(const Layer& layer,
             bool root_prefix,
             bool is_root,
             AvioFrameFacts& result) {
  const auto* metadata = layer.as_avio_frame_metadata_layer();
  if (metadata) {
    const auto& facts = metadata->facts();
    if (!root_prefix || !facts.IsValid() ||
        (result.item_opacity && facts.item_opacity) ||
        (result.ground_authored && facts.ground_authored) ||
        (result.ready_content_revision && facts.ready_content_revision)) {
      result.invalid = true;
    }
    if (facts.item_opacity) {
      result.item_opacity = facts.item_opacity;
      result.item_effect_declaration_id = facts.item_effect_declaration_id;
    }
    if (facts.ready_content_revision) {
      result.ready_content_revision = facts.ready_content_revision;
      result.ready_content_kind = facts.ready_content_kind;
    }
    if (facts.ground_authored) {
      result.ground_authored = true;
      result.ground_color_argb = facts.ground_color_argb;
      result.ground_regions = facts.ground_regions;
      result.ground_regions_count = facts.ground_regions_count;
    }
  }
  const auto* container = layer.as_container_layer();
  if (!container) {
    return;
  }
  const bool transparent_prefix =
      root_prefix && (is_root || metadata || layer.as_transform_layer()) &&
      container->layers().size() == 1u;
  for (const auto& child : container->layers()) {
    if (child->subtree_has_avio_frame_metadata()) {
      Collect(*child, transparent_prefix, false, result);
    }
  }
}
}  // namespace

AvioFrameFacts CollectAvioRootFrameFacts(const Layer& root) {
  AvioFrameFacts result;
  if (root.subtree_has_avio_frame_metadata()) {
    Collect(root, true, true, result);
  }
  return result;
}

}  // namespace flutter
