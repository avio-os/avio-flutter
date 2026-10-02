// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_AVIO_PIPELINE_PREWARM_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_AVIO_PIPELINE_PREWARM_H_
#include <functional>
#include "impeller/entity/contents/content_context_options.h"
namespace impeller {
enum class AvioPrewarmPipeline {
  kSolid,
  kTexture,
  kStrictTexture,
  kTiledTexture,
  kGlyph,
  kShadowVertices,
  kCircle,
  kUberSdf,
  kComplexRse,
  kFastGradient,
  kLinearGradient,
  kRadialGradient,
  kSweepGradient,
  kConicalGradient,
  kClip,
  kRRectBlur,
  kRSuperellipseBlur,
  kGaussianBlur,
  kBorderMaskBlur,
  kColorMatrix,
  kLinearToSrgb,
  kSrgbToLinear,
  kMorphology,
  kYuvToRgb,
  kDownsample,
  kDownsampleBounded,
  kDownsampleGles,
};
// The two 1x external/root formats and the actual native island format are
// explicit; native masks are warmed independently in their R8/S8 pass.
struct AvioPipelinePrewarmConfig {
  PixelFormat native_format;
  bool native_stencil_only = false;
  bool supports_ssbo = false;
  bool supports_triangle_fan = false;
  bool use_sdfs = false;
  bool continuous_sdf_cuts = false;
  bool needs_gles_decal_downsample = false;
};
// Gradient representation is selected by real device capabilities. Without
// SSBOs both short uniform stops and the overflow ramp texture are used.
enum class AvioPrewarmGradientStorage { kSsbo, kUniform, kTexture };
using AvioPipelinePrewarmCallback =
    std::function<bool(AvioPrewarmPipeline,
                       const ContentContextOptions&,
                       AvioPrewarmGradientStorage,
                       ConicalKind)>;
// Visits the bounded cold catalogue and stops on the first callback refusal.
// This is a bounded catalogue of common Shell drawing keys, not all possible
// application pipeline combinations. Uncatalogued keys remain counted.
bool PrewarmAvioPipelineKeys(const AvioPipelinePrewarmConfig& config,
                             const AvioPipelinePrewarmCallback& create);
// Queue every required key before joining any of them. The two phases visit
// the same authoritative catalogue; any refusal leaves initialization unready.
bool PrewarmAvioPipelineKeysInTwoPhases(
    const AvioPipelinePrewarmConfig& config,
    const AvioPipelinePrewarmCallback& queue,
    const AvioPipelinePrewarmCallback& validate);
}  // namespace impeller
#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_AVIO_PIPELINE_PREWARM_H_
