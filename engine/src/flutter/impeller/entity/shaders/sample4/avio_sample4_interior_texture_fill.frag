// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
precision highp float;
#include "sample4_clip.glsl"
#define main AvioOriginalSourceMain
#include "../texture_fill.frag"
#undef main
void main() {
  if (AvioJointClipMask4() != 15u)
    discard;
  AvioOriginalSourceMain();
  gl_SampleMask[0] = -1;
}
