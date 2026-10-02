// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IMPELLER_CLIP_COVERAGE_GLSL_
#define IMPELLER_CLIP_COVERAGE_GLSL_

// Bit identity matches kClipSampleLocations4. Caller coordinates are top-left,
// physical pixels on the original pass grid, even when drawing an atlas tile.
const highp vec2 kIPClipSampleLocations4[4] =
    vec2[4](vec2(0.375, 0.125), vec2(0.875, 0.375),
            vec2(0.125, 0.625), vec2(0.625, 0.875));

highp uint IPClipRectMask4(highp vec4 bounds, highp vec2 pixel_origin) {
  highp uint mask = 0u;
  for (highp uint i = 0u; i < 4u; i++) {
    highp vec2 sample_position = pixel_origin + kIPClipSampleLocations4[i];
    if (all(greaterThanEqual(sample_position, bounds.xy)) &&
        all(lessThan(sample_position, bounds.zw))) {
      mask |= 1u << i;
    }
  }
  return mask;
}

highp uint IPClipMask4FromSamples(highp vec4 binary_samples) {
  highp uint mask = 0u;
  for (highp uint i = 0u; i < 4u; i++) {
    if (binary_samples[i] > 0.5) {
      mask |= 1u << i;
    }
  }
  return mask;
}

// An alternative to unresolved binary R8 MSAA is bit-packed R8 UNorm. This
// stores mask/255, not resolved coverage; use nearest texel lookup only.
highp uint IPClipMask4FromR8(highp float encoded_mask) {
  return uint(round(encoded_mask * 255.0)) & 15u;
}

highp float IPClipMask4ToR8(highp uint mask) {
  return float(mask & 15u) / 255.0;
}

highp uint IPClipMask4Intersect(highp uint parent, highp uint shape) {
  return (parent & shape) & 15u;
}

highp uint IPClipMask4Difference(highp uint parent, highp uint shape) {
  return (parent & ~shape) & 15u;
}

highp float IPClipMask4Coverage(highp uint mask) {
  mask &= 15u;
  // No bitCount requirement: usable by the existing GLES3 shader targets too.
  return float((mask & 1u) + ((mask >> 1u) & 1u) +
               ((mask >> 2u) & 1u) + ((mask >> 3u) & 1u)) * 0.25;
}

bool IPClipMask4CoversSample(highp uint mask, highp uint sample_index) {
  return (mask & (1u << sample_index)) != 0u;
}

#endif  // IMPELLER_CLIP_COVERAGE_GLSL_
