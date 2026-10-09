// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IMPELLER_COVERAGE_GEOMETRY_GLSL_
#define IMPELLER_COVERAGE_GEOMETRY_GLSL_

#include <impeller/clip_coverage.glsl>

// CoverageConvexQuad4 uses the same (A,B,C,inclusive) columns on the CPU.
bool IPCoverageConvexQuadContains(vec2 point, mat4 lines) {
  for (int edge = 0; edge < 4; edge++) {
    float distance = dot(vec3(point, 1.0), lines[edge].xyz);
    if (distance < 0.0 || (distance == 0.0 && lines[edge].w == 0.0)) {
      return false;
    }
  }
  return true;
}

// Keep these bits until AND with the complete clip mask. Multiplying two
// resolved coverages loses identity even when both report the same alpha.
uint IPCoverageConvexQuadMask4(vec2 pixel_origin, mat4 lines) {
  uint mask = 0u;
  for (int sample_index = 0; sample_index < 4; sample_index++) {
    if (IPCoverageConvexQuadContains(
            pixel_origin + kIPClipSampleLocations4[sample_index], lines)) {
      mask |= 1u << uint(sample_index);
    }
  }
  return mask;
}

#endif  // IMPELLER_COVERAGE_GEOMETRY_GLSL_
