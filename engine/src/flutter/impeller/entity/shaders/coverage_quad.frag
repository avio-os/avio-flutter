// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

precision highp float;
precision highp int;

#include <impeller/coverage_geometry.glsl>

uniform QuadInfo {
  mat4 lines;
}
quad_info;

in highp vec2 v_logical_position;

void main() {
  // Preserve all four canonical Vulkan sample identities until stencil/depth
  // combines them with the parent clip. Never multiply resolved coverages.
  gl_SampleMask[0] = int(
      IPCoverageConvexQuadMask4(floor(v_logical_position), quad_info.lines));
}
