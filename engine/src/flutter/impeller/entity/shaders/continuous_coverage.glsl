// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef AVIO_CONTINUOUS_COVERAGE_GLSL_
#define AVIO_CONTINUOUS_COVERAGE_GLSL_

#include <impeller/coverage.glsl>
#include <impeller/types.glsl>
#include "continuous_clip.glsl"

struct AvioContinuousPrimitiveData {
  vec4 vectors[40];
};
layout(set = 0, binding = 29, std430) readonly buffer AvioContinuousControl {
  vec4 runtime;
  vec4 origin;
  vec4 factors;
  vec4 operations;
  AvioContinuousPrimitiveData own_geometry;
}
avio_control;
layout(set = 0, binding = 30, std430) readonly buffer AvioContinuousExpression {
  AvioContinuousPrimitiveData primitives[16];
}
avio_expression;
layout(set = 0, binding = 31) uniform sampler2DMS avio_destination;

// The analytic source shader can publish its own normalized distance and raw
// source before multiplying coverage. Nonanalytic geometry keeps the native
// rasterizer's original per-lane predicate.
float avio_geometry_distance = -1e20;
bool avio_has_geometry_distance = false;
float avio_external_linear_backdrop = 0.0;

vec4 AvioBlendFactor(int factor, vec4 source, vec4 destination) {
  if (factor == 0)
    return vec4(0.0);
  if (factor == 1)
    return vec4(1.0);
  if (factor == 2)
    return source;
  if (factor == 3)
    return vec4(1.0) - source;
  if (factor == 4)
    return vec4(source.a);
  if (factor == 5)
    return vec4(1.0 - source.a);
  if (factor == 6)
    return destination;
  if (factor == 7)
    return vec4(1.0) - destination;
  if (factor == 8)
    return vec4(destination.a);
  if (factor == 9)
    return vec4(1.0 - destination.a);
  if (factor == 10)
    return vec4(vec3(min(source.a, 1.0 - destination.a)), 1.0);
  // The entity coefficient family does not use constant blend colors. Such
  // descriptors are refused by the recorder rather than guessed here.
  return vec4(0.0);
}
vec4 AvioBlendOperation(vec4 source, vec4 destination, int operation) {
  if (operation == 1)
    return source - destination;
  if (operation == 2)
    return destination - source;
  return source + destination;
}
vec4 AvioApplyOriginalBlend(vec4 source, vec4 destination) {
  if (avio_control.operations.z < 0.5)
    return source;
  vec4 rgb = AvioBlendOperation(
      source *
          AvioBlendFactor(int(avio_control.factors.x), source, destination),
      destination *
          AvioBlendFactor(int(avio_control.factors.y), source, destination),
      int(avio_control.operations.x));
  vec4 alpha = AvioBlendOperation(
      source *
          AvioBlendFactor(int(avio_control.factors.z), source, destination),
      destination *
          AvioBlendFactor(int(avio_control.factors.w), source, destination),
      int(avio_control.operations.y));
  return clamp(vec4(rgb.rgb, alpha.a), vec4(0.0), vec4(1.0));
}
vec4 AvioApplyContinuousCoverage(vec4 source) {
  // gl_SampleID forces native per-sample execution. Explicit sample position
  // keeps the original grid/phase even with translated tile viewports.
  vec2 position =
      floor(gl_FragCoord.xy) + gl_SamplePosition + avio_control.origin.xy;
  float distance = avio_has_geometry_distance ? avio_geometry_distance : -1e20;
  // Slot -1 is the draw's own deferred geometry, then the ordered clip
  // expression. One loop gives the evaluator a single inlined call site.
  for (int slot = avio_control.runtime.y > 0.5 ? -1 : 0; slot < 16; ++slot) {
    if (slot >= 0 && slot >= int(avio_control.runtime.x))
      break;
    vec4 v[40];
    if (slot < 0) {
      v = avio_control.own_geometry.vectors;
    } else {
      v = avio_expression.primitives[slot].vectors;
    }
    float child = AvioContinuousDistance(v, position);
    if (slot >= 0 && v[0].y > 0.5)
      child = -child;
    distance = max(distance, child);
  }
  float coverage = 1.0 - smoothstep(-0.5, 0.5, distance);
  vec4 destination =
      texelFetch(avio_destination, ivec2(gl_FragCoord.xy), gl_SampleID);
  // Preserve the output's existing opacity/edge transfer after the final
  // combined source-over coverage, not before analytic clip intersection.
  bool source_over = avio_control.operations.z > 0.5 &&
                     avio_control.factors == vec4(1.0, 5.0, 1.0, 5.0) &&
                     avio_control.operations.xy == vec2(0.0);
  bool coerced_opaque_source_over =
      avio_control.operations.z < 0.5 && source.a >= 1.0;
  if (avio_external_linear_backdrop > 0.5 &&
      (source_over || coerced_opaque_source_over)) {
    vec4 covered_source =
        IPApplyExternalLinearBackdropCoverage(source * coverage, 1.0);
    return covered_source + destination * (1.0 - covered_source.a);
  }
  return mix(destination, AvioApplyOriginalBlend(source, destination),
             coverage);
}

#endif  // AVIO_CONTINUOUS_COVERAGE_GLSL_
