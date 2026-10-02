// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/geometry/coverage_geometry.h"

#include <bit>
#include <cmath>

namespace impeller {

std::optional<CoverageRasterKey4> CoverageRasterKey4::Make(
    const Matrix& transform,
    IRect raster_rect,
    const ClipSampleLocations4& locations) {
  if (raster_rect.IsEmpty()) {
    return std::nullopt;
  }
  CoverageRasterKey4 key;
  key.raster_rect = raster_rect;
  for (size_t index = 0; index < key.transform_bits.size(); index++) {
    if (!std::isfinite(transform.m[index])) {
      return std::nullopt;
    }
    key.transform_bits[index] = std::bit_cast<uint32_t>(transform.m[index]);
  }
  for (size_t index = 0; index < locations.size(); index++) {
    if (!locations[index].IsFinite() || locations[index].x < 0 ||
        locations[index].y < 0 || locations[index].x >= 1 ||
        locations[index].y >= 1) {
      return std::nullopt;
    }
    key.sample_location_bits[index * 2] =
        std::bit_cast<uint32_t>(locations[index].x);
    key.sample_location_bits[index * 2 + 1] =
        std::bit_cast<uint32_t>(locations[index].y);
  }
  return key;
}

std::optional<CoverageConvexQuad4> CoverageConvexQuad4::Make(const Quad& quad) {
  for (const auto& point : quad) {
    if (!point.IsFinite()) {
      return std::nullopt;
    }
  }
  const Quad perimeter = {quad[0], quad[1], quad[3], quad[2]};
  Scalar orientation = 0;
  for (size_t index = 0; index < perimeter.size(); index++) {
    auto turn = Point::Cross(perimeter[index], perimeter[(index + 1) % 4],
                             perimeter[(index + 2) % 4]);
    if (!std::isfinite(turn) || turn == 0 ||
        (orientation != 0 && (turn > 0) != (orientation > 0))) {
      return std::nullopt;
    }
    orientation = turn;
  }
  Matrix lines;
  for (size_t index = 0; index < perimeter.size(); index++) {
    auto start = perimeter[index];
    auto end = perimeter[(index + 1) % 4];
    if (orientation < 0) {
      std::swap(start, end);
    }
    auto delta = end - start;
    Scalar a = -delta.y;
    Scalar b = delta.x;
    Scalar c = -(a * start.x + b * start.y);
    if (!std::isfinite(c)) {
      return std::nullopt;
    }
    bool inclusive = delta.y < 0 || (delta.y == 0 && delta.x > 0);
    lines.vec[index] = Vector4{a, b, c, inclusive ? 1.0f : 0.0f};
  }
  return CoverageConvexQuad4(lines, Rect::MakePointBounds(quad).value());
}

CoverageConvexQuad4 CoverageConvexQuad4::Translated(Vector2 offset) const {
  auto lines = lines_;
  for (auto& line : lines.vec) {
    line.z -= line.x * offset.x + line.y * offset.y;
  }
  return CoverageConvexQuad4(lines, bounds_.Shift(offset));
}

std::optional<ClipSampleMask4> CoverageConvexQuad4::GetSampleMask(
    Point pixel_origin,
    const ClipSampleLocations4& locations) const {
  if (!pixel_origin.IsFinite() ||
      std::floor(pixel_origin.x) != pixel_origin.x ||
      std::floor(pixel_origin.y) != pixel_origin.y) {
    return std::nullopt;
  }
  ClipSampleMask4 mask = 0;
  for (size_t sample_index = 0; sample_index < locations.size();
       sample_index++) {
    const auto location = locations[sample_index];
    if (!location.IsFinite() || location.x < 0 || location.y < 0 ||
        location.x >= 1 || location.y >= 1) {
      return std::nullopt;
    }
    const auto point = pixel_origin + location;
    bool inside = true;
    for (const auto& line : lines_.vec) {
      Scalar distance = line.x * point.x + line.y * point.y + line.z;
      inside &= distance > 0 || (distance == 0 && line.w != 0);
    }
    if (inside) {
      mask |= 1u << sample_index;
    }
  }
  return mask;
}

}  // namespace impeller
