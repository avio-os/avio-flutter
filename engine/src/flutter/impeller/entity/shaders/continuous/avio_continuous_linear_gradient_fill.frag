// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

precision highp float;
#define AVIO_CONTINUOUS_COVERAGE 1
#include "../continuous_coverage.glsl"
#define main AvioOriginalSourceMain
#include "../gradients/linear_gradient_fill.frag"
#undef main
void main() {
  AvioOriginalSourceMain();
  frag_color = AvioApplyContinuousCoverage(vec4(frag_color));
}
