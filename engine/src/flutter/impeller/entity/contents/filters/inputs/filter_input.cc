// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/filters/inputs/filter_input.h"

#include <memory>
#include <utility>

#include "flutter/fml/logging.h"
#include "impeller/entity/contents/filters/filter_contents.h"
#include "impeller/entity/contents/filters/inputs/contents_filter_input.h"
#include "impeller/entity/contents/filters/inputs/filter_contents_filter_input.h"
#include "impeller/entity/contents/filters/inputs/placeholder_filter_input.h"
#include "impeller/entity/contents/filters/inputs/texture_filter_input.h"

namespace impeller {

FilterInput::Ref FilterInput::Make(Variant input,
                                   bool msaa_enabled,
                                   bool depth_stencil_enabled) {
  if (auto filter = std::get_if<std::shared_ptr<FilterContents>>(&input)) {
    return std::static_pointer_cast<FilterInput>(
        std::shared_ptr<FilterContentsFilterInput>(
            new FilterContentsFilterInput(*filter)));
  }

  if (auto contents = std::get_if<std::shared_ptr<Contents>>(&input)) {
    return std::static_pointer_cast<FilterInput>(
        std::shared_ptr<ContentsFilterInput>(new ContentsFilterInput(
            *contents, msaa_enabled, depth_stencil_enabled)));
  }

  if (auto texture = std::get_if<std::shared_ptr<Texture>>(&input)) {
    return Make(*texture, Matrix());
  }

  if (auto snapshot = std::get_if<Snapshot>(&input)) {
    return Make(*snapshot);
  }

  if (auto rect = std::get_if<Rect>(&input)) {
    return std::shared_ptr<PlaceholderFilterInput>(
        new PlaceholderFilterInput(*rect));
  }

  FML_UNREACHABLE();
}

FilterInput::Ref FilterInput::Make(std::shared_ptr<Texture> texture,
                                   Matrix local_transform) {
  return std::shared_ptr<TextureFilterInput>(new TextureFilterInput(
      Snapshot{.texture = std::move(texture), .transform = local_transform}));
}

FilterInput::Ref FilterInput::Make(Snapshot snapshot) {
  return std::shared_ptr<TextureFilterInput>(
      new TextureFilterInput(std::move(snapshot)));
}

FilterInput::Vector FilterInput::Make(std::initializer_list<Variant> inputs) {
  FilterInput::Vector result;
  result.reserve(inputs.size());
  for (const auto& input : inputs) {
    result.push_back(Make(input));
  }
  return result;
}

std::optional<Snapshot> FilterInput::GetSnapshotWithExactTextureExtent(
    std::string_view label,
    const ContentContext& renderer,
    const Entity& entity,
    std::optional<Rect> coverage_limit) const {
  auto snapshot = GetSnapshot(label, renderer, entity, coverage_limit);
  if (!snapshot || !snapshot->texture ||
      snapshot->GetTextureRect() ==
          Rect::MakeSize(snapshot->texture->GetSize())) {
    return snapshot;
  }
  auto source = Entity::FromSnapshot(*snapshot, BlendMode::kSrc);
  return source.GetContents()->Contents::RenderToSnapshot(
      renderer, source,
      {.msaa_enabled = false,
       .label = label,
       .coverage_expansion = 0,
       .depth_stencil_enabled = false,
       .exact_texture_extent = true});
}

Matrix FilterInput::GetLocalTransform(const Entity& entity) const {
  return Matrix();
}

std::optional<Rect> FilterInput::GetLocalCoverage(const Entity& entity) const {
  Entity local_entity = entity.Clone();
  local_entity.SetTransform(GetLocalTransform(entity));
  return GetCoverage(local_entity);
}

std::optional<Rect> FilterInput::GetSourceCoverage(
    const Matrix& effect_transform,
    const Rect& output_limit) const {
  return output_limit;
}

Matrix FilterInput::GetTransform(const Entity& entity) const {
  return entity.GetTransform() * GetLocalTransform(entity);
}

FilterInput::~FilterInput() = default;

void FilterInput::SetEffectTransform(const Matrix& matrix) {}

void FilterInput::SetRenderingMode(Entity::RenderingMode rendering_mode) {}

}  // namespace impeller
