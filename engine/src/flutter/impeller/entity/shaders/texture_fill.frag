// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

precision mediump float;

#include <impeller/constants.glsl>
#include <impeller/coverage.glsl>
#include <impeller/types.glsl>
#include "continuous_image_edge.glsl"
#ifdef IMPELLER_TARGET_VULKAN
#include <impeller/coverage_geometry.glsl>
#endif

uniform f16sampler2D texture_sampler;

uniform FragInfo {
  vec4 image_sample_rect;
  vec4 image_edge_rect;
  float continuous_image_edge;
  float analytic_quad;
  mat4 quad_lines;
  float alpha;
  float external_linear_backdrop;
}
frag_info;

#ifdef AVIO_CONTINUOUS_COVERAGE
sample in highp vec2 v_texture_coords;
#else
in highp vec2 v_texture_coords;
#endif

#ifdef IMPELLER_TARGET_VULKAN
noperspective in highp vec2 v_parent_position;
#endif
out f16vec4 frag_color;

void main() {
  float geometry_coverage = 1.0;
#ifdef IMPELLER_TARGET_VULKAN
  gl_SampleMask[0] = -1;
  if (frag_info.analytic_quad > .5) {
    uint mask = IPCoverageConvexQuadMask4(floor(v_parent_position),
                                          frag_info.quad_lines);
    if (frag_info.analytic_quad > 1.5) {
      geometry_coverage = float(bitCount(mask)) * .25;
    } else {
      gl_SampleMask[0] = int(mask);
    }
  }
#endif
  vec2 sample_uv = frag_info.continuous_image_edge > .5
                       ? clamp(v_texture_coords, frag_info.image_sample_rect.xy,
                               frag_info.image_sample_rect.zw)
                       : v_texture_coords;
  f16vec4 sampled =
      texture(texture_sampler, sample_uv, float16_t(kDefaultMipBias));
  frag_color = sampled * float16_t(frag_info.alpha);
#ifdef AVIO_CONTINUOUS_COVERAGE
  if (frag_info.continuous_image_edge > .5) {
    avio_geometry_distance =
        avioImageEdgeDistance(v_texture_coords, frag_info.image_edge_rect);
    avio_has_geometry_distance = true;
  }
  avio_external_linear_backdrop = frag_info.external_linear_backdrop;
#else
  frag_color = f16vec4(IPApplyExternalLinearBackdropCoverage(
      vec4(frag_color), frag_info.external_linear_backdrop));
#endif
  // Native4 evaluates the source transfer before hardware sample coverage.
  // This certified1x path keeps that order and resolves membership only once.
  frag_color *= float16_t(geometry_coverage);
}
