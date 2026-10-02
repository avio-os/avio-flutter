// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/filters/inputs/texture_filter_input.h"

#include <utility>

#include "impeller/core/formats.h"

namespace impeller {

TextureFilterInput::TextureFilterInput(Snapshot snapshot)
    : snapshot_(std::move(snapshot)) {}

TextureFilterInput::~TextureFilterInput() = default;

std::optional<Snapshot> TextureFilterInput::GetSnapshot(
    std::string_view label,
    const ContentContext& renderer,
    const Entity& entity,
    std::optional<Rect> coverage_limit,
    int32_t mip_count) const {
  auto snapshot = snapshot_;
  snapshot.transform = GetTransform(entity);
  if (snapshot.texture && snapshot.texture->GetMipCount() > 1) {
    snapshot.sampler_descriptor.label = "TextureFilterInput Trilinear Sampler";
    snapshot.sampler_descriptor.mip_filter = MipFilter::kLinear;
  }
  return snapshot;
}

std::optional<Rect> TextureFilterInput::GetCoverage(
    const Entity& entity) const {
  auto snapshot = snapshot_;
  snapshot.transform = GetTransform(entity);
  return snapshot.GetCoverage();
}

Matrix TextureFilterInput::GetLocalTransform(const Entity& entity) const {
  return snapshot_.transform;
}

}  // namespace impeller
