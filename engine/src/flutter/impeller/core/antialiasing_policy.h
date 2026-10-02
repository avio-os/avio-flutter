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
inline constexpr uint64_t kAvioContinuousSupportedClasses = 0u;

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
  constexpr bool operator==(const AvioAntialiasingConfig&) const = default;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_CORE_ANTIALIASING_POLICY_H_
