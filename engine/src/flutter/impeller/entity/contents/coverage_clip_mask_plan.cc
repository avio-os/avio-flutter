// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/coverage_clip_mask_flattener.h"

#include <limits>

#include "impeller/core/texture.h"

namespace impeller {
namespace {
bool ValidNativeMask(const CoverageMaskTile& mask) {
  if (!mask.lease || !mask.lease->IsValid() ||
      mask.lease->GetKind() != AvioCoverageRegion::Kind::kMask ||
      !mask.target_rect.IsFinite() || mask.target_rect.IsEmpty() ||
      mask.target_rect != Rect::Make(IRect::RoundOut(mask.target_rect)) ||
      mask.target_rect.GetSize() != Size(mask.lease->GetRequestedSize())) {
    return false;
  }
  auto texture = mask.lease->GetRenderTarget().GetColorAttachment(0).texture;
  return texture &&
         texture->GetTextureDescriptor().format == PixelFormat::kR8UNormInt &&
         texture->GetTextureDescriptor().sample_count == SampleCount::kCount4 &&
         (texture->GetTextureDescriptor().usage & TextureUsage::kShaderRead);
}

}  // namespace

PreparedFillMaskStatus ValidateClipMaskTile(const AvioSample4ClipRecipe& recipe,
                                            IRect raster,
                                            ISize scratch_size) {
  if (recipe.logical_pass_size.IsEmpty() || raster.IsEmpty() ||
      scratch_size.IsEmpty() || raster.GetWidth() > scratch_size.width ||
      raster.GetHeight() > scratch_size.height) {
    return PreparedFillMaskStatus::kFailed;
  }
  // Saved bundles are on one physical pass grid. Reusing a descriptor from
  // another layer/extent cannot be repaired by reinterpreting its old MVP.
  size_t bundles = 0;
  for (auto current = &recipe; current; current = current->parent.get()) {
    if (++bundles > AvioSample4ClipDescriptor::kMaxBundles ||
        current->logical_pass_size != recipe.logical_pass_size) {
      return PreparedFillMaskStatus::kFailed;
    }
  }
  // Tiles keep integral raster padding beyond the writable parent edge. The
  // caller clips its write ROI; clipping this source rectangle would move the
  // native sample phase and change the later atlas UV mapping.
  bool empty =
      !IRect::MakeSize(recipe.logical_pass_size).IntersectsWithRect(raster);
  size_t count = 0;
  bool valid = recipe.ForEachNode([&](const AvioSample4ClipNode& node) {
    count++;
    if (node.clip_depth >= std::numeric_limits<uint32_t>::max() ||
        !node.GetBounds().IsFinite()) {
      return false;
    }
    if (node.operation == ClipOperation::kIntersect &&
        (!node.GetBounds().IntersectsWithRect(Rect::Make(raster)) ||
         node.GetBounds().IsEmpty())) {
      empty = true;
    }
    // Geometry is authoritative when both retained winding and a cached
    // representation are present. A missing, non-empty tessellation is an
    // error, not an implicit identity clip.
    if (node.geometry) {
      return node.geometry->transform.IsFinite() &&
             (node.geometry->vertex_buffer || node.GetBounds().IsEmpty());
    }
    if (node.quad) {
      return true;
    }
    return node.mask && ValidNativeMask(*node.mask);
  });
  if (!valid || count == 0) {
    return PreparedFillMaskStatus::kFailed;
  }
  return empty ? PreparedFillMaskStatus::kEmpty
               : PreparedFillMaskStatus::kPrepared;
}

Matrix ClipMaskReplayTransform(ISize logical_pass_size,
                               ISize scratch_size,
                               IPoint raster_origin) {
  // MakeOrthographic deliberately has a zero z scale and is singular. Undo
  // only its physical x/y projection; a general Matrix::Invert would return
  // identity and silently move every retained clip to the wrong pixel grid.
  const auto half_width = Scalar(logical_pass_size.width) * .5f;
  const auto half_height = Scalar(logical_pass_size.height) * .5f;
  const auto to_physical =
      Matrix::MakeTranslation({half_width, half_height, 0}) *
      Matrix::MakeScale(Vector3{half_width, -half_height, 1});
  return Matrix::MakeOrthographic(scratch_size) *
         Matrix::MakeTranslation(
             {-Scalar(raster_origin.x), -Scalar(raster_origin.y), 0}) *
         to_physical;
}

}  // namespace impeller
