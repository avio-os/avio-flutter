// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/coverage_path_atlas.h"

#include "flutter/display_list/geometry/dl_path.h"
#include "impeller/entity/contents/coverage_mask_cache.h"
#include "impeller/entity/geometry/coverage_geometry.h"
#include "impeller/renderer/blit_pass.h"
#include "impeller/renderer/command_buffer.h"
#include "impeller/renderer/context.h"
#include "impeller/renderer/render_pass.h"

namespace impeller {
namespace {

bool ValidTileLease(const AvioCoverageRegion::Lease& lease,
                    const CoverageAtlasTile& tile) {
  return lease.IsValid() && !tile.raster_rect.IsEmpty() &&
         tile.raster_rect.Contains(tile.content_rect) &&
         lease.GetRequestedSize() == tile.raster_rect.GetSize();
}

Matrix TileTransform(const Matrix& transform,
                     const AvioCoverageRegion::Lease& lease,
                     const CoverageAtlasTile& tile) {
  auto delta =
      lease.GetContentRect().GetOrigin() - tile.raster_rect.GetOrigin();
  return Matrix::MakeTranslation(
             {static_cast<Scalar>(delta.x), static_cast<Scalar>(delta.y), 0}) *
         transform;
}

}  // namespace

struct CoveragePathAtlas::PathCache {
  CoverageMaskCache4<flutter::DlPath, AvioCoverageRegion::Lease> entries;
};

CoveragePathAtlas::CoveragePathAtlas(std::shared_ptr<AvioCoverageRegion> region,
                                     std::shared_ptr<Context> context)
    : region_(std::move(region)),
      context_(std::move(context)),
      path_cache_(std::make_unique<PathCache>()) {}

CoveragePathAtlas::~CoveragePathAtlas() = default;

bool CoveragePathAtlas::Initialize() {
  if (!region_ || !context_) {
    return false;
  }
  return region_->InitializeMaskOnce([&] {
    auto command_buffer = context_->CreateCommandBuffer();
    auto target = region_->GetCoverageAtlasTarget();
    if (!command_buffer || target.GetRenderArea().has_value() ||
        target.GetSampleCount() != SampleCount::kCount4) {
      return false;
    }
    // The full target clears R8 native samples and S8 winding scratch
    // once. Subsequent leases clear only their explicit padded area.
    auto pass = command_buffer->CreateRenderPass(target);
    if (!pass || !pass->EncodeCommands()) {
      return false;
    }
    auto blit = command_buffer->CreateBlitPass();
    auto texture = target.GetColorAttachment(0).texture;
    return blit && texture && blit->ConvertTextureToShaderRead(texture) &&
           blit->EncodeCommands() &&
           context_->EnqueueCommandBuffer(std::move(command_buffer));
  });
}

std::optional<AvioCoverageRegion::Acquisition>
CoveragePathAtlas::AcquirePathMask(
    const flutter::DlPath& path,
    const Matrix& physical_transform,
    const CoverageAtlasTile& tile,
    const std::function<bool(const AvioCoverageRegion::Lease&, const Matrix&)>&
        render_mask) {
  auto raster_key =
      CoverageRasterKey4::Make(physical_transform, tile.raster_rect);
  if (!region_ || !context_ || !raster_key.has_value() || !render_mask ||
      tile.raster_rect.IsEmpty() ||
      !tile.raster_rect.Contains(tile.content_rect)) {
    return std::nullopt;
  }
  // The region's native four-sample mask must stay unresolved. A resolved
  // scalar would discard the identity needed for nested clip intersection.
  if (region_->GetCoverageAtlasTarget().GetSampleCount() !=
      SampleCount::kCount4) {
    return std::nullopt;
  }
  auto& entries = path_cache_->entries;
  // Generation speeds equality; exact path equality verifies it, so a
  // generation wrap cannot alias different shapes or winding rules.
  auto generation = path.GetSkPath().getGenerationID();
  if (auto lease = entries.Find(path, generation, raster_key.value())) {
    return AvioCoverageRegion::Acquisition{AvioCoverageRegion::Status::kSuccess,
                                           std::move(lease)};
  }
  if (!entries.CanInsert()) {
    return AvioCoverageRegion::Acquisition{
        AvioCoverageRegion::Status::kNeedsFlush, nullptr};
  }
  auto acquired = region_->AcquireCoverage(AvioCoverageRegion::Kind::kMask,
                                           tile.raster_rect.GetSize());
  if (acquired.status != AvioCoverageRegion::Status::kSuccess) {
    return acquired;
  }
  if (!acquired.lease || !ValidTileLease(*acquired.lease, tile) ||
      !render_mask(*acquired.lease,
                   TileTransform(physical_transform, *acquired.lease, tile))) {
    return std::nullopt;
  }
  auto command_buffer = context_->CreateCommandBuffer();
  if (!command_buffer) {
    return std::nullopt;
  }
  auto blit = command_buffer->CreateBlitPass();
  auto texture =
      acquired.lease->GetRenderTarget().GetColorAttachment(0).texture;
  if (!blit || !texture || !blit->ConvertTextureToShaderRead(texture) ||
      !blit->EncodeCommands() ||
      !context_->EnqueueCommandBuffer(std::move(command_buffer))) {
    return std::nullopt;
  }
  if (!entries.Insert(path, generation, raster_key.value(), acquired.lease)) {
    return std::nullopt;
  }
  return acquired;
}

void CoveragePathAtlas::ClearPathCache() {
  path_cache_->entries.Clear();
}

}  // namespace impeller
