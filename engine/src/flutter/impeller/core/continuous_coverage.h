// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef FLUTTER_IMPELLER_CORE_CONTINUOUS_COVERAGE_H_
#define FLUTTER_IMPELLER_CORE_CONTINUOUS_COVERAGE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace impeller {

// Copied with a recorded draw. No pointers into the mutable Canvas clip stack.
// A bounded expression fails admission on overflow; it never drops a parent.
inline constexpr size_t kAvioContinuousMaxClips = 16;
inline constexpr size_t kAvioContinuousPrimitiveVectors = 40;
// Impellerc reserves a fragment-stage offset64. GLSL29/30/31 reflect
// as Vulkan93/94/95; original shader resource bindings remain unchanged.
inline constexpr uint32_t kAvioContinuousControlBinding = 93;
inline constexpr uint32_t kAvioContinuousUniformBinding = 94;
inline constexpr uint32_t kAvioContinuousDestinationBinding = 95;
using AvioContinuousVector = std::array<float, 4>;

enum class AvioContinuousPrimitiveType : uint32_t {
  kRect = 0,
  kRoundedRect = 1,
  kSuperellipse = 2,
  kOval = 3,
  kEmpty = 4,
  kBorderedRoundedRect = 5,
  kArc = 6,
};

struct alignas(16) AvioContinuousPrimitive {
  // vec[0]: type, difference(0/1), reserved, reserved.
  // vec[1..2]: inverse affine rows (x, y, translation, 0).
  // vec[3]: local center.xy, half-size.xy.
  // vec[4..5]: elliptical corner radii x/y, BR, TR, BL, TL.
  // vec[6]: superellipse split coordinates top,bottom,left,right.
  // vec[7]: all-corners-same, reserved...
  // Each quadrant (TR,BR,BL,TL) occupies 8 vectors at 8+8*q:
  // offset.xy/signed-scale.xy; top offset.xy/a/n;
  // top circle-center.xy/radius/angle; right offset.xy/a/n;
  // right circle-center.xy/radius/angle; remaining vectors reserved.
  std::array<AvioContinuousVector, kAvioContinuousPrimitiveVectors> vectors{};
  bool operator==(const AvioContinuousPrimitive&) const = default;
};

struct AvioContinuousClipExpression {
  uint32_t count = 0;
  std::array<AvioContinuousPrimitive, kAvioContinuousMaxClips> primitives{};

  bool Append(const AvioContinuousPrimitive& primitive, bool difference) {
    if (primitive.vectors[0][0] ==
        static_cast<float>(AvioContinuousPrimitiveType::kEmpty)) {
      if (!difference) {
        count = 1;
        primitives[0] = primitive;
        primitives[0].vectors[0][1] = 0.f;
      }
      return true;
    }
    // Contradictory identical shapes have an empty set, not a faint SDF rim.
    // Comparison includes every transform/physical-phase and shape parameter.
    for (uint32_t i = 0; i < count; i++) {
      if (primitives[i].vectors[0][0] ==
          static_cast<float>(AvioContinuousPrimitiveType::kEmpty)) {
        return true;
      }
      auto existing = primitives[i];
      const bool old_difference = existing.vectors[0][1] != 0.f;
      existing.vectors[0][1] = primitive.vectors[0][1];
      if (existing.vectors == primitive.vectors) {
        if (old_difference == difference) {
          return true;
        }
        count = 1;
        primitives[0] = {};
        primitives[0].vectors[0][0] =
            static_cast<float>(AvioContinuousPrimitiveType::kEmpty);
        return true;
      }
    }
    if (count == primitives.size()) {
      return false;
    }
    primitives[count] = primitive;
    primitives[count++].vectors[0][1] = difference ? 1.f : 0.f;
    return true;
  }
  bool IsEmpty() const { return count == 0; }
};
static_assert(std::is_trivially_copyable_v<AvioContinuousClipExpression>);
static_assert(sizeof(AvioContinuousPrimitive) == 40 * 16);

}  // namespace impeller
#endif  // FLUTTER_IMPELLER_CORE_CONTINUOUS_COVERAGE_H_
