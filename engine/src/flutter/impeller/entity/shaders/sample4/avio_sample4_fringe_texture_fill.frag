// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
precision highp float;
#include "sample4_clip.glsl"
#define main AvioOriginalSourceMain
#include "../texture_fill.frag"
#undef main
void main() {
  uint mask = AvioJointClipMask4();
  if (mask == 0u || mask == 15u) discard;
  AvioOriginalSourceMain();
  // The captured source independently proves geometry on every surviving
  // clip lane. Keep those lanes through the original destination operator.
  gl_SampleMask[0] = int(mask);
}
