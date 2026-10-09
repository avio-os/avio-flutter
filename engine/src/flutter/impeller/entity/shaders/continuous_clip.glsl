// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef AVIO_CONTINUOUS_CLIP_GLSL_
#define AVIO_CONTINUOUS_CLIP_GLSL_
#include "sdf_functions.glsl"
float avioRectDistance(vec2 p, vec2 half_size) {
  vec2 q = abs(p) - half_size;
  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
}
float avioOvalDistance(vec2 p, vec2 radii) {
  radii = max(radii, vec2(0.00001));
  p = abs(p);
  if (dot(p, p) < 1e-12) {
    return -min(radii.x, radii.y);
  }
  float t = atan(p.y * radii.x, p.x * radii.y);
  for (int i = 0; i < 8; i++) {
    vec2 cs = vec2(cos(t), sin(t));
    vec2 a = radii * cs, v = radii * vec2(-cs.y, cs.x);
    float denominator = dot(v, v) + dot(p - a, a);
    if (abs(denominator) > 1e-10) {
      t = clamp(t + clamp(dot(p - a, v) / denominator, -0.25, 0.25), 0.0,
                PI * 0.5);
    }
  }
  float d = length(p - radii * vec2(cos(t), sin(t)));
  return dot(p / radii, p / radii) > 1.0 ? d : -d;
}
float avioRoundedRectDistance(vec2 p, vec2 half_size, vec4 rx, vec4 ry) {
  int corner = p.x > 0.0 ? (p.y > 0.0 ? 0 : 1) : (p.y > 0.0 ? 2 : 3);
  vec2 radius = vec2(rx[corner], ry[corner]);
  vec2 q = abs(p) - half_size + radius;
  if (min(radius.x, radius.y) < 0.00001 || min(q.x, q.y) <= 0.0) {
    return avioRectDistance(p, half_size);
  }
  return avioOvalDistance(q, radius);
}
float avioSuperellipseOctant(vec2 p, vec4 octant, vec4 circle) {
  p -= octant.xy;
  if (octant.w < 2.0) {
    return max(p.x, p.y) - octant.z;
  }
  vec2 rel = p - circle.xy;
  float delta = mod(atan(rel.y, rel.x) - PI_OVER_FOUR + PI, TWO_PI) - PI;
  if (abs(delta) < abs(circle.w)) {
    return length(rel) - circle.z;
  }
  return sdSuperellipse(p / max(octant.z, 0.00001), octant.w) * octant.z;
}
float avioSuperellipseDistance(vec4 v[40], vec2 position) {
  vec2 center = v[3].xy, half_size = v[3].zw, p = position - center;
  int quadrant = 0;
  bool symmetric = v[7].x > 0.5;
  if (!symmetric) {
    vec4 splits = v[6] - vec4(center.x, center.x, center.y, center.y);
    vec2 T = vec2(splits.x, -half_size.y), R = vec2(half_size.x, splits.w);
    vec2 B = vec2(splits.y, half_size.y), L = vec2(-half_size.x, splits.z);
    float t = T.x * p.y - T.y * p.x, r = R.x * p.y - R.y * p.x;
    float b = B.x * p.y - B.y * p.x, l = L.x * p.y - L.y * p.x;
    if ((r < 0.0 || r == 0.0 && p.x > 0.0) &&
        (t > 0.0 || t == 0.0 && p.x > 0.0)) {
      quadrant = 0;
    } else if ((b < 0.0 || b == 0.0 && p.x > 0.0) &&
               (r > 0.0 || r == 0.0 && p.x > 0.0)) {
      quadrant = 1;
    } else if (b >= 0.0 && l <= 0.0) {
      quadrant = 2;
    } else {
      quadrant = 3;
    }
  }
  int base = 8 + 8 * quadrant;
  vec4 frame = v[base];
  vec2 q = (position - frame.xy) / frame.zw;
  if (symmetric) {
    q = abs(q);
  }
  vec4 top = v[base + 1], right = v[base + 3];
  // Both octants describe one continuous corner. Select at their shared
  // diagonal, then combine the straight bounding edges without alpha products.
  float c = -top.y;
  float d = q.y + c > q.x
                ? avioSuperellipseOctant(q, top, v[base + 2])
                : avioSuperellipseOctant(q.yx, right.yxzw, v[base + 4]);
  return max(avioRectDistance(p, half_size),
             d * min(abs(frame.z), abs(frame.w)));
}
#include "analytic_arc.glsl"

float AvioContinuousLocalDistance(vec4 v[40], vec2 position) {
  vec2 p = position - v[3].xy;
  if (v[0].x < 0.5) {
    return avioRectDistance(p, v[3].zw);
  }
  if (v[0].x < 1.5) {
    return avioRoundedRectDistance(p, v[3].zw, v[4], v[5]);
  }
  if (v[0].x < 2.5) {
    return avioSuperellipseDistance(v, position);
  }
  if (v[0].x < 3.5) {
    return avioOvalDistance(p, v[3].zw);
  }
  if (v[0].x < 4.5) {
    return 1e20;
  }
  float d;
  if (v[0].x < 5.5) {
    float outer = avioRoundedRectDistance(p, v[3].zw, v[4], v[5]);
    float inner =
        min(v[8].z, v[8].w) > 0.0
            ? avioRoundedRectDistance(position - v[8].xy, v[8].zw, v[9], v[10])
            : 1e20;
    d = max(outer, -inner);
    if (v[12].y > 0.5) {
      float minimum_width = max(length(vec2(dFdx(d), dFdy(d))), 1e-6);
      d = abs(d) - max(v[12].x, minimum_width) * .5;
    }
  } else {
    float base = avioArcFillDistance(p, v[3].zw, v[11]);
    float minimum_width = max(length(vec2(dFdx(base), dFdy(base))), 1e-6);
    d = v[12].y > 0.5 ? avioArcStrokeDistance(
                            p, v[3].zw, v[11],
                            vec3(max(v[12].x, minimum_width), v[12].z, v[12].w))
                      : base;
  }
  return d;
}
// Return physical pixel distance; Boolean operations combine distances before
// a single smooth coverage evaluation. The original pass grid survives tiling.
// Impellerc inlines every call, so the local evaluator has exactly one call
// site: tap 0 is the value and taps 1-4 are the +dx, -dx, +dy, -dy central
// differences, each formed by the same single add or subtract as before.
float AvioContinuousDistance(vec4 v[40], vec2 physical_position) {
  vec3 position = vec3(physical_position, 1.0);
  vec2 local = vec2(dot(v[1].xyz, position), dot(v[2].xyz, position));
  vec2 dx = vec2(v[1].x, v[2].x) * 0.01, dy = vec2(v[1].y, v[2].y) * 0.01;
  float taps[5];
  for (int tap = 0; tap < 5; ++tap) {
    vec2 offset = tap < 3 ? dx : dy;
    vec2 tap_position = tap == 0                 ? local
                        : (tap == 1 || tap == 3) ? local + offset
                                                 : local - offset;
    taps[tap] = AvioContinuousLocalDistance(v, tap_position);
  }
  vec2 gradient = vec2(taps[1] - taps[2], taps[3] - taps[4]) / 0.02;
  return taps[0] / max(length(gradient), 0.00001);
}
#endif
