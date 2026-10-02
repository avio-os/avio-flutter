// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <impeller/types.glsl>

uniform FrameInfo {
  mat4 mvp;
  float depth;
} frame_info;

in vec2 position;
out highp vec2 v_logical_position;

void main() {
  gl_Position = frame_info.mvp * vec4(position, 0.0, 1.0);
  gl_Position.z = frame_info.depth * gl_Position.w;
  // A tiled replay shifts the viewport, not this original logical pixel grid.
  // gl_FragCoord would instead identify the atlas-local destination pixel.
  v_logical_position = position;
}
