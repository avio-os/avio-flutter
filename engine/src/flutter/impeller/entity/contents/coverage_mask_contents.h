// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_COVERAGE_MASK_CONTENTS_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_COVERAGE_MASK_CONTENTS_H_

#include <memory>
#include <optional>
#include <span>

#include "impeller/entity/avio_coverage_region.h"
#include "impeller/entity/clip_operation.h"
#include "impeller/geometry/rect.h"

namespace impeller {
class ContentContext;
class Entity;
class Geometry;
class RenderPass;
class CoverageConvexQuad4;
struct Matrix;

enum class PreparedFillMaskStatus {
  kNotApplicable,
  kPrepared,
  kEmpty,
  kFailed
};

struct CoverageMaskTile {
  std::shared_ptr<AvioCoverageRegion::Lease> lease;
  // Pass-relative physical pixels, same size as the mask's content rectangle.
  // Integer origins preserve native sample IDs. Retain the whole raster tile;
  // a clipped write ROI is applied by the destination pass's scissor.
  Rect target_rect;
  // Original geometry bounds for classified work-area accounting.
  Rect shape_bounds;
};

struct AcquiredCoverageMask {
  PreparedFillMaskStatus status;
  std::optional<CoverageMaskTile> mask;
};

class CoverageMaskContents {
 public:
  // Prepare/cache binary geometry only; never mutates the destination pass.
  static AcquiredCoverageMask TryAcquireClipPathMask(
      const ContentContext& renderer,
      const Matrix& physical_transform,
      ISize logical_pass_size,
      const Geometry& geometry);

  static PreparedFillMaskStatus TryPrepareFillPath(
      const ContentContext& renderer,
      const Entity& entity,
      RenderPass& pass,
      const Geometry& geometry);

  // Emit binary native sample membership to stencil without changing depth.
  // The original source shader and blend then render with kCoverCompare.
  static bool PrepareStencil(const ContentContext& renderer,
                             RenderPass& pass,
                             std::span<const CoverageMaskTile> masks,
                             Scalar shader_depth);
  // Analytic transformed rectangle coverage on the original pass pixel grid.
  // Both shape and parent retain native sample identity until depth is written.
  static bool RenderQuadClip(const ContentContext& renderer,
                             RenderPass& pass,
                             const CoverageConvexQuad4& quad,
                             uint32_t clip_depth,
                             ClipOperation operation);

  // Combine every tile into one clip before writing clip depth. Applying an
  // intersect clip separately for each tile would incorrectly AND the tiles.
  static bool RenderClip(const ContentContext& renderer,
                         RenderPass& pass,
                         std::span<const CoverageMaskTile> masks,
                         uint32_t clip_depth,
                         ClipOperation operation);
};

}  // namespace impeller
#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_COVERAGE_MASK_CONTENTS_H_
