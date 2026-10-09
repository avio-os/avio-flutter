// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef AVIO_CONTINUOUS_IMAGE_EDGE_GLSL_
#define AVIO_CONTINUOUS_IMAGE_EDGE_GLSL_
float avioImageEdgeDistance(vec2 uv, vec4 bounds) {
  vec2 delta =
      abs(uv - (bounds.xy + bounds.zw) * .5) - (bounds.zw - bounds.xy) * .5;
  vec2 pixel_size = vec2(length(vec2(dFdx(uv.x), dFdy(uv.x))),
                         length(vec2(dFdx(uv.y), dFdy(uv.y))));
  delta /= max(pixel_size, vec2(1e-12));
  return length(max(delta, 0.0)) + min(max(delta.x, delta.y), 0.0);
}
#endif
