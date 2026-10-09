// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
precision highp float;
#include "sample4_clip.glsl"
#define main AvioOriginalSourceMain
#include "../solid_fill.frag"
#undef main
void main() {
  uint mask = AvioJointClipMask4();
  // The corresponding 1x pass already owns every all-lanes interior pixel.
  // Keep partial lanes independent through the original destination blend.
  if (mask == 0u || mask == 15u)
    discard;
  gl_SampleMask[0] = int(mask);
  AvioOriginalSourceMain();
}
