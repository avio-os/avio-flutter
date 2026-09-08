// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/external_coverage_contents.h"

#include "impeller/entity/contents/texture_contents.h"
#include "impeller/entity/entity.h"
#include "impeller/renderer/render_pass.h"

namespace impeller {

ExternalCoverageContents::ExternalCoverageContents(
    std::shared_ptr<Contents> source)
    : source_(std::move(source)) {}

std::optional<Rect> ExternalCoverageContents::GetCoverage(
    const Entity& entity) const {
  return opacity_ <= 0.0f ? std::nullopt : source_->GetCoverage(entity);
}

void ExternalCoverageContents::SetInheritedOpacity(Scalar opacity) {
  opacity_ = opacity;
}

bool ExternalCoverageContents::Render(const ContentContext& renderer,
                                      const Entity& entity,
                                      RenderPass& pass) const {
  if (entity.GetBlendMode() != BlendMode::kSrcOver) {
    source_->SetInheritedOpacity(opacity_);
    return source_->Render(renderer, entity, pass);
  }
  auto coverage = GetCoverage(entity);
  if (!coverage.has_value()) {
    return true;
  }
  // Clip allocations to the destination. Round out in physical coordinates so
  // snapshotting does not translate fractional geometry onto a different grid.
  auto limit = Rect::MakeSize(pass.GetRenderTargetSize());
  auto hint = GetCoverageHint();
  if (hint.has_value()) {
    auto intersection = limit.Intersection(hint.value());
    if (!intersection.has_value()) {
      return true;
    }
    limit = intersection.value();
  }
  if (!coverage->Expand(1).Intersection(limit).has_value()) {
    return true;
  }
  auto snapshot =
      source_->Contents::RenderToSnapshot(renderer, entity,
                                          {.coverage_limit = limit,
                                           .label = "External coverage mask",
                                           .pixel_aligned = true});
  if (!snapshot.has_value()) {
    return false;
  }
  auto rect = Rect::MakeSize(snapshot->texture->GetSize());
  auto contents = TextureContents::MakeRect(rect);
  contents->SetTexture(snapshot->texture);
  contents->SetSourceRect(rect);
  contents->SetOpacity(opacity_ * snapshot->opacity);
  contents->SetCoverageMode(flutter::DlCoverageMode::kExternalLinearBackdrop);
  contents->SetLabel("External coverage resolve");
  Entity resolved;
  resolved.SetBlendMode(entity.GetBlendMode());
  resolved.SetClipDepth(entity.GetClipDepth());
  resolved.SetTransform(snapshot->transform);
  resolved.SetContents(contents);
  return contents->Render(renderer, resolved, pass);
}

}  // namespace impeller
