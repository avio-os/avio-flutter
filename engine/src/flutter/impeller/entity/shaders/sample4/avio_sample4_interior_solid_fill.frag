// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
precision highp float;
#include "sample4_clip.glsl"
#define main AvioOriginalSourceMain
#include "../solid_fill.frag"
#undef main
void main() {
  // All four native lanes agree: original source and blend run once at 1x.
  // No source alpha is multiplied by a second clip coverage.
  if (AvioJointClipMask4() != 15u)
    discard;
  AvioOriginalSourceMain();
}
