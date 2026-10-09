// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef FLUTTER_IMPELLER_ENTITY_GEOMETRY_GEOMETRY_RESULT_H_
#define FLUTTER_IMPELLER_ENTITY_GEOMETRY_GEOMETRY_RESULT_H_
#include "impeller/core/formats.h"
#include "impeller/core/vertex_buffer.h"
#include "impeller/geometry/matrix.h"
namespace impeller {
struct GeometryResult {
  enum class Mode {
    /// The geometry has no overlapping triangles.
    kNormal,
    /// The geometry may have overlapping triangles. The geometry should be
    /// stenciled with the NonZero fill rule.
    kNonZero,
    /// The geometry may have overlapping triangles. The geometry should be
    /// stenciled with the EvenOdd fill rule.
    kEvenOdd,
    /// The geometry may have overlapping triangles, but they should not
    /// overdraw or cancel each other out. This is a special case for stroke
    /// geometry.
    kPreventOverdraw,
  };

  PrimitiveType type = PrimitiveType::kTriangleStrip;
  VertexBuffer vertex_buffer;
  Matrix transform;
  Mode mode = Mode::kNormal;
};

}  // namespace impeller
#endif  // FLUTTER_IMPELLER_ENTITY_GEOMETRY_GEOMETRY_RESULT_H_
