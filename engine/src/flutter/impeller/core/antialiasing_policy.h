// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_CORE_ANTIALIASING_POLICY_H_
#define FLUTTER_IMPELLER_CORE_ANTIALIASING_POLICY_H_

#include <cstddef>
#include <cstdint>

namespace impeller {

enum class AvioAntialiasingPolicy : uint32_t { kMsaa4 = 0, kCoverage = 1 };

// The source integrates the explicit coverage clip/path/region policy.
// ABI negotiation and validation share this implementation fact; runtime
// device support is checked independently before resource initialization.
inline constexpr bool kAvioCoveragePolicyImplemented = true;
// Stable class identities are independent of each backend's implementation.
enum class AvioContinuousClass : uint64_t {
  kRectClip = 1ull << 0,
  kRoundedRectClip = 1ull << 1,
  kSuperellipseClip = 1ull << 2,
  kOvalClip = 1ull << 3,
  kBorderedRoundedRect = 1ull << 4,
  kArc = 1ull << 5,
  kImageEdge = 1ull << 6,
};
inline constexpr uint64_t kAvioContinuousKnownClasses = (1ull << 7) - 1;
enum class AvioCoverageBackend { kVulkan, kGLES, kMetal };
// Implemented source classes do not imply physical/logical-device support or
// changed-look approval. Backends never inherit another backend's mask.
constexpr uint64_t AvioContinuousSupportedClasses(AvioCoverageBackend backend) {
  return backend == AvioCoverageBackend::kVulkan ? kAvioContinuousKnownClasses
                                                 : 0u;
}
inline constexpr uint64_t kAvioContinuousSupportedClasses =
    AvioContinuousSupportedClasses(AvioCoverageBackend::kVulkan);

struct AvioAntialiasingConfig {
  AvioAntialiasingPolicy policy = AvioAntialiasingPolicy::kMsaa4;
  uint32_t layer_sample_count = 4u;
  uint32_t coverage_sample_count = 4u;
  uint64_t continuous_requested_classes = 0u;
  size_t coverage_region_max_bytes = 0u;
  size_t layer_region_max_bytes = 0u;

  constexpr bool UsesCoverage() const {
    return policy == AvioAntialiasingPolicy::kCoverage;
  }
  constexpr bool RequestsContinuous(AvioContinuousClass shape_class) const {
    return UsesCoverage() && (continuous_requested_classes &
                              static_cast<uint64_t>(shape_class)) != 0;
  }
  constexpr bool operator==(const AvioAntialiasingConfig&) const = default;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_CORE_ANTIALIASING_POLICY_H_
