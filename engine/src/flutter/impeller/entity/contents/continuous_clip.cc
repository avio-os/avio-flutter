// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "impeller/entity/contents/continuous_clip.h"
#include <algorithm>
#include <cmath>
#include "impeller/entity/contents/uber_sdf_parameters.h"
#include "impeller/geometry/round_superellipse_param.h"
namespace impeller {
namespace {
AvioContinuousPrimitive MakePrimitive(AvioContinuousPrimitiveType type,
                                      const Rect& bounds) {
  AvioContinuousPrimitive p;
  p.vectors[0][0] = static_cast<float>(
      bounds.IsEmpty() ? AvioContinuousPrimitiveType::kEmpty : type);
  p.vectors[1] = {1, 0, 0, 0};
  p.vectors[2] = {0, 1, 0, 0};
  const auto c = bounds.GetCenter();
  const auto s = bounds.GetSize() * .5f;
  p.vectors[3] = {c.x, c.y, s.width, s.height};
  return p;
}
void SetRadii(AvioContinuousPrimitive& p, const RoundingRadii& r) {
  p.vectors[4] = {r.bottom_right.width, r.top_right.width, r.bottom_left.width,
                  r.top_left.width};
  p.vectors[5] = {r.bottom_right.height, r.top_right.height,
                  r.bottom_left.height, r.top_left.height};
}
void SetQuadrant(AvioContinuousPrimitive& p,
                 size_t index,
                 RoundSuperellipseParam::Quadrant q) {
  const auto b = 8 + 8 * index;
  p.vectors[b] = {q.offset.x, q.offset.y, q.signed_scale.x, q.signed_scale.y};
  p.vectors[b + 1] = {q.top.offset.x, q.top.offset.y, q.top.se_a, q.top.se_n};
  p.vectors[b + 2] = {q.top.circle_center.x, q.top.circle_center.y,
                      q.top.circle_radius, q.top.circle_max_angle.radians};
  p.vectors[b + 3] = {q.right.offset.x, q.right.offset.y, q.right.se_a,
                      q.right.se_n};
  p.vectors[b + 4] = {q.right.circle_center.x, q.right.circle_center.y,
                      q.right.circle_radius, q.right.circle_max_angle.radians};
}
}  // namespace
std::optional<AvioContinuousClip> AvioContinuousClip::Geometry(
    const UberSDFParameters& params) {
  if (params.type != UberSDFParameters::Type::kArc &&
      params.type != UberSDFParameters::Type::kBorderedRoundedRect) {
    return std::nullopt;
  }
  AvioContinuousPrimitive p;
  const bool arc = params.type == UberSDFParameters::Type::kArc;
  p.vectors[0][0] = static_cast<float>(
      arc ? AvioContinuousPrimitiveType::kArc
          : AvioContinuousPrimitiveType::kBorderedRoundedRect);
  p.vectors[1] = {1, 0, 0, 0};
  p.vectors[2] = {0, 1, 0, 0};
  p.vectors[3] = {params.center.x, params.center.y, params.size.x,
                  params.size.y};
  p.vectors[4] = {params.radii.x, params.radii.y, params.radii.z,
                  params.radii.w};
  p.vectors[5] = {params.bordered_radii_y.x, params.bordered_radii_y.y,
                  params.bordered_radii_y.z, params.bordered_radii_y.w};
  p.vectors[8] = {params.inner_center.x, params.inner_center.y,
                  params.inner_size.x, params.inner_size.y};
  p.vectors[9] = {params.inner_radii.x, params.inner_radii.y,
                  params.inner_radii.z, params.inner_radii.w};
  p.vectors[10] = {params.inner_radii_y.x, params.inner_radii_y.y,
                   params.inner_radii_y.z, params.inner_radii_y.w};
  p.vectors[11] = {params.arc.x, params.arc.y, params.arc.z, params.arc.w};
  const float join = !params.stroke || params.stroke->join == Join::kMiter ? 0.f
                     : params.stroke->join == Join::kBevel ? 1.f
                                                           : 2.f;
  p.vectors[12] = {params.stroke ? params.stroke->width : 0,
                   params.stroke ? 1.f : 0.f, join,
                   params.stroke ? params.stroke->miter_limit : 4.f};
  return AvioContinuousClip{arc ? AvioContinuousClass::kArc
                                : AvioContinuousClass::kBorderedRoundedRect,
                            p};
}

AvioContinuousClip AvioContinuousClip::RectClip(const Rect& r) {
  return {AvioContinuousClass::kRectClip,
          MakePrimitive(AvioContinuousPrimitiveType::kRect, r)};
}
AvioContinuousClip AvioContinuousClip::OvalClip(const Rect& r) {
  return {AvioContinuousClass::kOvalClip,
          MakePrimitive(AvioContinuousPrimitiveType::kOval, r)};
}
AvioContinuousClip AvioContinuousClip::RoundRectClip(const RoundRect& r) {
  auto p =
      MakePrimitive(AvioContinuousPrimitiveType::kRoundedRect, r.GetBounds());
  SetRadii(p, r.GetRadii());
  return {AvioContinuousClass::kRoundedRectClip, p, r.GetRadii()};
}
AvioContinuousClip AvioContinuousClip::SuperellipseClip(
    const RoundSuperellipse& r) {
  auto p =
      MakePrimitive(AvioContinuousPrimitiveType::kSuperellipse, r.GetBounds());
  if (r.GetBounds().IsEmpty()) {
    return {AvioContinuousClass::kSuperellipseClip, p, r.GetRadii()};
  }
  const auto params =
      RoundSuperellipseParam::MakeBoundsRadii(r.GetBounds(), r.GetRadii());
  p.vectors[6] = {params.top_split, params.bottom_split, params.left_split,
                  params.right_split};
  p.vectors[7][0] = params.all_corners_same ? 1 : 0;
  SetQuadrant(p, 0, params.top_right);
  if (!params.all_corners_same) {
    SetQuadrant(p, 1, params.bottom_right);
    SetQuadrant(p, 2, params.bottom_left);
    SetQuadrant(p, 3, params.top_left);
  }
  return {AvioContinuousClass::kSuperellipseClip, p, r.GetRadii()};
}
std::optional<AvioContinuousPrimitive> AvioContinuousClip::Transform(
    const Matrix& transform) const {
  if (!transform.IsAffine() || !transform.IsFinite() ||
      !transform.IsInvertible()) {
    return std::nullopt;
  }
  for (const auto& vector : primitive.vectors) {
    for (const auto value : vector) {
      if (!std::isfinite(value)) {
        return std::nullopt;
      }
    }
  }
  const auto inverse = transform.Invert();
  const Point o = inverse * Point(0, 0), x = inverse * Point(1, 0),
              y = inverse * Point(0, 1);
  auto p = primitive;
  p.vectors[1] = {x.x - o.x, y.x - o.x, o.x, 0};
  p.vectors[2] = {x.y - o.y, y.y - o.y, o.y, 0};
  return p;
}
float CombineAvioContinuousDistance(float parent,
                                    float child,
                                    bool difference) {
  return std::max(parent, difference ? -child : child);
}
float AvioContinuousCoverage(float d) {
  const float t = std::clamp(d + .5f, 0.f, 1.f);
  return 1.f - t * t * (3.f - 2.f * t);
}
}  // namespace impeller
