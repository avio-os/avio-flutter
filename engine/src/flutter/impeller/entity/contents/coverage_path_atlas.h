// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_COVERAGE_PATH_ATLAS_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_COVERAGE_PATH_ATLAS_H_

#include <functional>
#include <memory>
#include <optional>

#include "impeller/entity/avio_coverage_region.h"
#include "impeller/entity/contents/coverage_atlas.h"
#include "impeller/geometry/matrix.h"

namespace flutter {
class DlPath;
}  // namespace flutter

namespace impeller {

class Context;

// Raster-owner cache. Cached mask leases remain immutable across frames and
// are evicted explicitly when the fixed region requires a batch flush.
class CoveragePathAtlas {
 public:
  CoveragePathAtlas(std::shared_ptr<AvioCoverageRegion> region,
                    std::shared_ptr<Context> context);
  ~CoveragePathAtlas();
  CoveragePathAtlas(const CoveragePathAtlas&) = delete;
  CoveragePathAtlas& operator=(const CoveragePathAtlas&) = delete;

  // Cold, context-owned initialization. Partial atlas writes must preserve
  // defined native samples outside their render area, including on frame 1.
  bool Initialize();

  // Caches immutable fill-path masks in the fixed region. render_mask must
  // produce an opaque binary value per native sample, without source alpha,
  // filters, or an already-resolved clip. Stroke contents use colour islands;
  // their minimum-width alpha compensation is not a binary mask.
  // Nullopt is an invalid key or a failed render. Region pressure is returned
  // explicitly so the owner can clear cache references and flush safely.
  // The callback must encode and enqueue its writes on this context's queue
  // before returning; the atlas then orders a real shader-read barrier.
  std::optional<AvioCoverageRegion::Acquisition> AcquirePathMask(
      const flutter::DlPath& path,
      const Matrix& physical_transform,
      const CoverageAtlasTile& tile,
      const std::function<bool(const AvioCoverageRegion::Lease&,
                               const Matrix&)>& render_mask);

  // Clears cache references only. Retained clips/commands still own their
  // leases and continue to prevent region overwrite.
  void ClearPathCache();

 private:
  struct PathCache;
  std::shared_ptr<AvioCoverageRegion> region_;
  std::shared_ptr<Context> context_;
  std::unique_ptr<PathCache> path_cache_;
};

}  // namespace impeller
#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_COVERAGE_PATH_ATLAS_H_
