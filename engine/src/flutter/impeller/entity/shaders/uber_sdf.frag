// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// UberSDF with gradients sampled from a gradient ramp texture, for backends
// without storage buffers.

#include "uber_sdf_common.glsl"

uniform sampler2D color_source_sampler;

vec4 getGradientColor(highp float t) {
  // UberSDF always uses a transparent decal border.
  return IPSampleLinearWithTileMode(color_source_sampler, vec2(t, 0.5),
                                    frag_info.half_texel, frag_info.tile_mode,
                                    vec4(0.0));
}

vec4 finishGradientColor(vec4 premultiplied_color) {
  // The ramp texture gradient shaders do not dither.
  return premultiplied_color;
}
