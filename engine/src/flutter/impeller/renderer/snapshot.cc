// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/snapshot.h"

#include <optional>

#include "impeller/renderer/render_target.h"

namespace impeller {

Snapshot Snapshot::FromRenderTarget(
    const RenderTarget& target,
    Matrix transform,
    std::optional<SamplerDescriptor> sampler,
    Scalar opacity,
    bool needs_rasterization_for_runtime_effects) {
  Snapshot snapshot;
  snapshot.texture = target.GetRenderTargetTexture();
  snapshot.transform = transform;
  if (target.GetContentRect()) {
    snapshot.texture_rect = Rect::Make(*target.GetContentRect());
    snapshot.transform = transform * Matrix::MakeTranslation(
                                         -snapshot.texture_rect->GetOrigin());
  }
  if (sampler) {
    snapshot.sampler_descriptor = *sampler;
  }
  snapshot.opacity = opacity;
  snapshot.needs_rasterization_for_runtime_effects =
      needs_rasterization_for_runtime_effects;
  snapshot.resource_owner = target.GetResourceOwner();
  return snapshot;
}

Rect Snapshot::GetTextureRect() const {
  return texture_rect.value_or(texture ? Rect::MakeSize(texture->GetSize())
                                       : Rect());
}

std::optional<Rect> Snapshot::GetCapturedOpaqueRect() const {
  if (!IsImmutableCapturedBackdrop() || opacity != 1.f ||
      !captured_opaque_texels || !captured_opaque_texels->IsFinite() ||
      !GetCoverage()) {
    return std::nullopt;
  }
  return captured_opaque_texels->Intersection(GetTextureRect());
}

bool Snapshot::IsImmutableCapturedBackdrop() const {
  return is_immutable_captured_backdrop && resource_owner && GetCoverage();
}

std::optional<Rect> Snapshot::GetCoverage() const {
  if (!texture) {
    return std::nullopt;
  }
  const auto rect = GetTextureRect();
  if (rect.IsEmpty() || !Rect::MakeSize(texture->GetSize()).Contains(rect)) {
    return std::nullopt;
  }
  return rect.TransformBounds(transform);
}

std::optional<Matrix> Snapshot::GetUVTransform() const {
  if (!texture || texture->GetSize().IsEmpty() || !GetCoverage()) {
    return std::nullopt;
  }
  return Matrix::MakeScale(1 / Vector2(texture->GetSize())) *
         transform.Invert();
}

std::optional<std::array<Point, 4>> Snapshot::GetCoverageUVs(
    const Rect& coverage) const {
  auto uv_transform = GetUVTransform();
  if (!uv_transform.has_value()) {
    return std::nullopt;
  }
  return coverage.GetTransformedPoints(uv_transform.value());
}

}  // namespace impeller
