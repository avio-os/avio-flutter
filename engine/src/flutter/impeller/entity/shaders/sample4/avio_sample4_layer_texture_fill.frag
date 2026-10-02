// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
precision highp float;
#include "sample4_clip.glsl"

// Preserve the original TextureFill binding and FragInfo ABI. This private
// composite consumes only the standing RGBA8/BGRA8 1x attachment with the exact
// nearest/base sampler; its original sources have already been shaded/stored.
uniform highp sampler2D texture_sampler;
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

in highp vec2 v_texture_coords;
out highp vec4 frag_color;

void main() {
  // Sampling decodes BGRA storage through its declared format. Reconstruct the
  // stored UNORM codes in highp before coverage, instead of introducing the
  // original source shader's f16 approximation of an already encoded byte.
  vec4 encoded = round(textureLod(texture_sampler, v_texture_coords, 0.0) *
                       255.0) / 255.0;
  frag_color = encoded * (frag_info.alpha * AvioResolveJointClip4());
}
