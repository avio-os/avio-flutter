// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <impeller/types.glsl>
#include <impeller/coverage_geometry.glsl>
struct AvioSample4Node { vec4 lines[4]; vec4 target; vec4 raster; vec4 meta; };
layout(set=0,binding=29,std430) readonly buffer AvioSample4ClipControl {
  vec4 runtime; vec4 origin; AvioSample4Node nodes[4];
} avio_sample4;
layout(set=0,binding=30) uniform sampler2DMS avio_sample4_mask0;
layout(set=0,binding=31) uniform sampler2DMS avio_sample4_mask1;
layout(set=0,binding=32) uniform sampler2DMS avio_sample4_mask2;
layout(set=0,binding=33) uniform sampler2DMS avio_sample4_mask3;
float AvioSample4MaskTexel(int index, ivec2 pixel, int sample_index) {
  if (index==0) return texelFetch(avio_sample4_mask0,pixel,sample_index).r;
  if (index==1) return texelFetch(avio_sample4_mask1,pixel,sample_index).r;
  if (index==2) return texelFetch(avio_sample4_mask2,pixel,sample_index).r;
  return texelFetch(avio_sample4_mask3,pixel,sample_index).r;
}
uint AvioJointClipMask4() {
  vec2 pixel=floor(gl_FragCoord.xy)+avio_sample4.origin.xy;
  uint mask=15u;
  for(int index=0;index<4;++index) {
    if(index>=int(avio_sample4.runtime.x)) break;
    AvioSample4Node node=avio_sample4.nodes[index];
    uint shape=0u;
    if(node.meta.x<.5) {
      shape=IPCoverageConvexQuadMask4(pixel,mat4(node.lines[0],node.lines[1],node.lines[2],node.lines[3]));
    } else {
      vec2 local=pixel-node.target.xy;
      if(all(greaterThanEqual(local,vec2(0))) && all(lessThan(local,node.target.zw))) {
        ivec2 atlas_pixel=ivec2(local+node.raster.xy);
        for(int sample_index=0;sample_index<4;++sample_index) {
          if(AvioSample4MaskTexel(index,atlas_pixel,sample_index)>.5)
            shape |= 1u<<uint(sample_index);
        }
      }
    }
    mask &= node.meta.y>.5 ? (~shape & 15u) : shape;
  }
  return mask;
}
float AvioResolveJointClip4() { return float(bitCount(AvioJointClipMask4()))*.25; }
