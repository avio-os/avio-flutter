// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_PORTER_DUFF_BLEND_COEFFICIENTS_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_PORTER_DUFF_BLEND_COEFFICIENTS_H_

#include <array>
#include <vector>

#include "impeller/geometry/color.h"

namespace impeller {

// Premultiplied shader equation: src * (c[0] + dst.a * c[1]) +
// dst * (c[2] + src.a * c[3] + src * c[4]). Screen simplifies to
// src + dst * (1 - src), including alpha. Keep one coefficient authority for
// shader specialization and its CPU oracle instead of separate tables.
constexpr std::array<std::array<Scalar, 5>, 15> kPorterDuffCoefficients = {{
    {0, 0, 0, 0, 0},    // Clear
    {1, 0, 0, 0, 0},    // Source
    {0, 0, 1, 0, 0},    // Destination
    {1, 0, 1, -1, 0},   // SourceOver
    {1, -1, 1, 0, 0},   // DestinationOver
    {0, 1, 0, 0, 0},    // SourceIn
    {0, 0, 0, 1, 0},    // DestinationIn
    {1, -1, 0, 0, 0},   // SourceOut
    {0, 0, 1, -1, 0},   // DestinationOut
    {0, 1, 1, -1, 0},   // SourceATop
    {1, -1, 0, 1, 0},   // DestinationATop
    {1, -1, 1, -1, 0},  // Xor
    {1, 0, 1, 0, 0},    // Plus
    {0, 0, 0, 0, 1},    // Modulate
    {1, 0, 1, 0, -1},   // Screen
}};

static_assert(kPorterDuffCoefficients.size() ==
              static_cast<size_t>(kLastCoefficientBlendMode) + 1);

inline std::array<std::vector<Scalar>, kPorterDuffCoefficients.size()>
GetPorterDuffSpecConstants(bool supports_decal) {
  std::array<std::vector<Scalar>, kPorterDuffCoefficients.size()> result;
  for (size_t i = 0; i < result.size(); i++) {
    result[i].reserve(6);
    result[i].push_back(supports_decal ? 1 : 0);
    result[i].insert(result[i].end(), kPorterDuffCoefficients[i].begin(),
                     kPorterDuffCoefficients[i].end());
  }
  return result;
}

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_PORTER_DUFF_BLEND_COEFFICIENTS_H_
