// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef AVIO_ANALYTIC_ARC_GLSL_
#define AVIO_ANALYTIC_ARC_GLSL_
float avioCross(vec2 a, vec2 b) { return a.x*b.y-a.y*b.x; }
float avioSegmentDistance(vec2 p, vec2 a, vec2 b) {
  vec2 v=b-a;
  return length(p-a-v*clamp(dot(p-a,v)/max(dot(v,v),1e-12),0.0,1.0));
}
vec2 avioArcPoint(vec2 radii, float angle) { return radii*vec2(cos(angle),sin(angle)); }
float avioArcFillDistance(vec2 p, vec2 radii, vec4 arc) {
  float ellipse=avioOvalDistance(p,radii);
  if (arc.y>=TWO_PI) {return ellipse;}
  if (arc.y<=0.0) {return 1e20;}
  vec2 start=avioArcPoint(radii,arc.x), end=avioArcPoint(radii,arc.x+arc.y);
  if (arc.z>0.5) {
    float a=-avioCross(start,p)/max(length(start),1e-6);
    float b=avioCross(end,p)/max(length(end),1e-6);
    return max(ellipse, arc.y<=PI ? max(a,b):min(a,b));
  }
  vec2 chord=end-start;
  vec2 midpoint=avioArcPoint(radii,arc.x+arc.y*.5);
  float side=sign(avioCross(chord,midpoint-start));
  float cut=-side*avioCross(chord,p-start)/max(length(chord),1e-6);
  return max(ellipse,cut);
}
// Closest point on the actual ellipse segment, not a circle scaled after
// distance evaluation. Endpoints are included in the bounded minimization.
vec3 avioClosestArcPoint(vec2 p, vec2 radii, vec4 arc) {
  float sweep=min(arc.y,TWO_PI);
  if (dot(p,p)<1e-12) {
    // atan(0,0) is undefined. The center's closest point is an endpoint or
    // one of the ellipse's four axis extrema within this exact arc.
    float best_angle=0.0;
    float best_distance=length(avioArcPoint(radii,arc.x));
    float end_distance=length(avioArcPoint(radii,arc.x+sweep));
    if (end_distance<best_distance) {best_angle=sweep;best_distance=end_distance;}
    for (int i=0;i<4;i++) {
      float candidate=mod(float(i)*PI*.5-arc.x,TWO_PI);
      if (candidate<=sweep) {
        float distance=length(avioArcPoint(radii,arc.x+candidate));
        if (distance<best_distance) {best_angle=candidate;best_distance=distance;}
      }
    }
    return vec3(avioArcPoint(radii,arc.x+best_angle),best_angle);
  }
  float angle=mod(atan(p.y/radii.y,p.x/radii.x)-arc.x,TWO_PI);
  if (angle>sweep) {angle=length(p-avioArcPoint(radii,arc.x))<length(p-avioArcPoint(radii,arc.x+sweep)) ? 0.0:sweep;}
  for (int i=0;i<8;i++) {
    vec2 cs=vec2(cos(arc.x+angle),sin(arc.x+angle));
    vec2 q=radii*cs, tangent=radii*vec2(-cs.y,cs.x);
    float denominator=dot(tangent,tangent)+dot(p-q,q);
    if (abs(denominator)>1e-10) {
      angle=clamp(angle+clamp(dot(p-q,tangent)/denominator,-.25,.25),0.0,sweep);
    }
  }
  vec2 q=avioArcPoint(radii,arc.x+angle);
  vec2 start=avioArcPoint(radii,arc.x), end=avioArcPoint(radii,arc.x+sweep);
  if (length(p-start)<length(p-q)) {q=start;angle=0.0;}
  if (length(p-end)<length(p-q)) {q=end;angle=sweep;}
  return vec3(q,angle);
}
float avioOpenArcStrokeDistance(vec2 p, vec2 radii, vec4 arc, float width) {
  float half_width=width*.5;
  if (arc.y<=0.0) {return 1e20;}
  vec3 closest=avioClosestArcPoint(p,radii,arc);
  float d=length(p-closest.xy)-half_width;
  if (arc.y>=TWO_PI || arc.w>0.5 && arc.w<1.5) {return d;}
  // Butt and square caps clip the stroked ellipse by the exact endpoint
  // tangent planes. A square cap adds half the stroke along the tangent.
  vec2 start=avioArcPoint(radii,arc.x), end=avioArcPoint(radii,arc.x+arc.y);
  vec2 ts=normalize(radii*vec2(-sin(arc.x),cos(arc.x)));
  vec2 te=normalize(radii*vec2(-sin(arc.x+arc.y),cos(arc.x+arc.y)));
  float extension=arc.w>1.5 ? half_width:0.0;
  if (closest.z<1e-5) {
    vec2 q=vec2(-dot(p-start,ts)-extension,abs(avioCross(ts,p-start))-half_width);
    d=length(max(q,0.0))+min(max(q.x,q.y),0.0);
  }
  if (closest.z>arc.y-1e-5) {
    vec2 q=vec2(dot(p-end,te)-extension,abs(avioCross(te,p-end))-half_width);
    d=length(max(q,0.0))+min(max(q.x,q.y),0.0);
  }
  return d;
}
float avioStrokeSegment(vec2 p, vec2 a, vec2 b, float width) {
  vec2 ab=b-a; float span=length(ab);
  if (span<1e-8) {return 1e20;}
  vec2 direction=ab/span;
  vec2 position=vec2(dot(p-a,direction)-span*.5,avioCross(direction,p-a));
  vec2 delta=abs(position)-vec2(span*.5,width*.5);
  return length(max(delta,0.0))+min(max(delta.x,delta.y),0.0);
}
float avioTriangleDistance(vec2 p, vec2 a, vec2 b, vec2 c) {
  float orientation=sign(avioCross(b-a,c-a));
  if (orientation==0.0) {return 1e20;}
  float d=min(avioSegmentDistance(p,a,b),min(avioSegmentDistance(p,b,c),avioSegmentDistance(p,c,a)));
  bool inside=orientation*avioCross(b-a,p-a)>=0.0 &&
      orientation*avioCross(c-b,p-b)>=0.0 && orientation*avioCross(a-c,p-c)>=0.0;
  return inside ? -d:d;
}
float avioStrokeJoin(vec2 p, vec2 vertex, vec2 incoming, vec2 outgoing, vec3 stroke) {
  float width=stroke.x, kind=stroke.y, limit=stroke.z;
  float turn=sign(avioCross(incoming,outgoing));
  if (kind>1.5) {return length(p-vertex)-width*.5;}
  if (turn==0.0) {return 1e20;}
  vec2 n0=vec2(incoming.y,-incoming.x)*turn;
  vec2 n1=vec2(outgoing.y,-outgoing.x)*turn;
  vec2 a=vertex+n0*width*.5, b=vertex+n1*width*.5;
  float bevel=avioTriangleDistance(p,vertex,a,b);
  float denominator=dot(n0+n1,n0);
  if (kind>.5 || abs(denominator)<1e-6) {return bevel;}
  vec2 miter=(n0+n1)*(width*.5/denominator);
  if (length(miter)>limit*width*.5) {return bevel;}
  return min(bevel,avioTriangleDistance(p,a,vertex+miter,b));
}
float avioArcStrokeDistance(vec2 p, vec2 radii, vec4 arc, vec3 stroke) {
  if (arc.z<.5) {return avioOpenArcStrokeDistance(p,radii,arc,stroke.x);}
  // The closed sector consists of the actual ellipse segment and two radial
  // lines. Join geometry is evaluated before coverage, including miter limits;
  // it is not an annular alpha applied to the filled sector's resolved mask.
  vec2 start=avioArcPoint(radii,arc.x), end=avioArcPoint(radii,arc.x+arc.y);
  vec2 start_direction=normalize(start), end_direction=normalize(end);
  vec2 start_tangent=normalize(radii*vec2(-sin(arc.x),cos(arc.x)));
  vec2 end_tangent=normalize(radii*vec2(-sin(arc.x+arc.y),cos(arc.x+arc.y)));
  vec4 open_arc=vec4(arc.xy,0.0,0.0);
  float d=avioOpenArcStrokeDistance(p,radii,open_arc,stroke.x);
  d=min(d,avioStrokeSegment(p,vec2(0),start,stroke.x));
  d=min(d,avioStrokeSegment(p,end,vec2(0),stroke.x));
  d=min(d,avioStrokeJoin(p,start,start_direction,start_tangent,stroke));
  d=min(d,avioStrokeJoin(p,end,end_tangent,-end_direction,stroke));
  d=min(d,avioStrokeJoin(p,vec2(0),-end_direction,start_direction,stroke));
  return d;
}
#endif
