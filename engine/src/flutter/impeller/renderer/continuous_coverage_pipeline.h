// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_CONTINUOUS_COVERAGE_PIPELINE_H_
#define FLUTTER_IMPELLER_RENDERER_CONTINUOUS_COVERAGE_PIPELINE_H_

#include "impeller/core/continuous_coverage.h"
#include "impeller/renderer/pipeline.h"

namespace impeller {

// This std430 control block is small and varies with a draw/tile. The much
// larger immutable primitive array is bound separately and uploaded once per
// clip-stack state, rather than once per replayed draw.
struct alignas(16) AvioContinuousControl {
  AvioContinuousVector runtime{};  // clip count, own-geometry-present.
  AvioContinuousVector origin{};   // original-pass raster origin x/y.
  AvioContinuousVector
      factors{};  // source RGB, destination RGB, source A, dest A.
  AvioContinuousVector operations{};  // RGB/A operation, blending enabled.
  AvioContinuousPrimitive own_geometry{};
};
static_assert(sizeof(AvioContinuousControl) == 704);

// Returns the original-source Vulkan shader variant which reads an immutable
// native4 destination prefix and applies the original operator at fractional
// coverage. Missing variants and reserved-binding collisions fail closed.
std::shared_ptr<Pipeline<PipelineDescriptor>> GetAvioContinuousPipeline(
    const Context& context,
    const PipelineDescriptor& original);

AvioContinuousControl MakeAvioContinuousControl(
    const ColorAttachmentDescriptor& original,
    uint32_t clip_count,
    IPoint raster_origin,
    const AvioContinuousPrimitive* own_geometry = nullptr);

}  // namespace impeller
#endif  // FLUTTER_IMPELLER_RENDERER_CONTINUOUS_COVERAGE_PIPELINE_H_
