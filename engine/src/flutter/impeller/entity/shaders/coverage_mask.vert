// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <impeller/types.glsl>

uniform FrameInfo {
  mat4 mvp;
  float depth;
} frame_info;

in vec2 position;
in vec2 texture_coords;
out highp vec2 v_texture_coords;

void main() {
  gl_Position = frame_info.mvp * vec4(position, 0.0, 1.0);
  gl_Position.z = frame_info.depth * gl_Position.w;
  // Unnormalized physical texel coordinates. The whole atlas size is never
  // substituted for the tile's explicitly retained content rectangle.
  v_texture_coords = texture_coords;
}
