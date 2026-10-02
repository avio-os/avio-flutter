// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_CONTINUOUS_CLIP_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_CONTINUOUS_CLIP_H_
#include <optional>
#include "impeller/core/antialiasing_policy.h"
#include "impeller/core/continuous_coverage.h"
#include "impeller/geometry/matrix.h"
#include "impeller/geometry/round_rect.h"
#include "impeller/geometry/round_superellipse.h"
namespace impeller {
struct UberSDFParameters;
struct AvioContinuousClip {
  AvioContinuousClass shape_class;
  AvioContinuousPrimitive primitive;
  // Native sample4 fringe planning uses the original normalized corner
  // radii. These are CPU facts, independent of continuous shader packing.
  RoundingRadii sample4_radii;
  static std::optional<AvioContinuousClip> Geometry(
      const UberSDFParameters& params);
  static AvioContinuousClip RectClip(const Rect& bounds);
  static AvioContinuousClip OvalClip(const Rect& bounds);
  static AvioContinuousClip RoundRectClip(const RoundRect& bounds);
  static AvioContinuousClip SuperellipseClip(const RoundSuperellipse& bounds);
  // Original pass-relative physical grid, never tile-relative. Singular or
  // perspective transforms cannot represent this affine packet and fail closed.
  std::optional<AvioContinuousPrimitive> Transform(
      const Matrix& transform) const;
};
// CPU oracle for ordered correlated intersection/difference. Values are signed
// distances in physical pixels; the final smooth coverage is evaluated once.
float CombineAvioContinuousDistance(float parent, float child, bool difference);
float AvioContinuousCoverage(float signed_pixel_distance);
}  // namespace impeller
#endif
