// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <impeller/types.glsl>

uniform FrameInfo {
  mat4 mvp;
  vec2 parent_size;
}
frame_info;

in vec2 position;
in vec2 texture_coords;

out highp vec2 v_texture_coords;
#ifdef IMPELLER_TARGET_VULKAN
noperspective out highp vec2 v_parent_position;
#endif

void main() {
  gl_Position = frame_info.mvp * vec4(position, 0.0, 1.0);
  v_texture_coords = texture_coords;
#ifdef IMPELLER_TARGET_VULKAN
  vec2 ndc=gl_Position.xy/gl_Position.w;
  v_parent_position=vec2((ndc.x+1.0)*.5,(1.0-ndc.y)*.5)*frame_info.parent_size;
#endif
}
