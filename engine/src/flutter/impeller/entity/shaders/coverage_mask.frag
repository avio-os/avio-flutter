// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

precision highp float;
precision highp int;

#include <impeller/clip_coverage.glsl>

uniform highp sampler2DMS mask_sampler;
in highp vec2 v_texture_coords;

void main() {
  ivec2 pixel = ivec2(floor(v_texture_coords));
  vec4 membership = vec4(texelFetch(mask_sampler, pixel, 0).r,
                         texelFetch(mask_sampler, pixel, 1).r,
                         texelFetch(mask_sampler, pixel, 2).r,
                         texelFetch(mask_sampler, pixel, 3).r);
  // No gl_SampleID input and no sampleRateShading feature are required. One
  // fragment invocation emits the exact four-bit fixed-function sample mask.
  // Stencil/depth sees the intersection with native geometry/parent coverage.
  gl_SampleMask[0] = int(IPClipMask4FromSamples(membership));
}
