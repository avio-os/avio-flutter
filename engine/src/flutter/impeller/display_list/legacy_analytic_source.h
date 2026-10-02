// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_DISPLAY_LIST_LEGACY_ANALYTIC_SOURCE_H_
#define FLUTTER_IMPELLER_DISPLAY_LIST_LEGACY_ANALYTIC_SOURCE_H_

#include "impeller/core/antialiasing_policy.h"

namespace impeller {

enum class LegacyAnalyticSourceRoute {
  kGeometry,
  kLegacyPass,
  kCoverageNative4,
  kCoverageDirect1x,
};

// Source selection and target sample count are separate decisions. Mixed or
// clipped scopes retain the original analytic shader in bounded native4 replay;
// only the complete scope proof authorizes a one-sample target.
constexpr LegacyAnalyticSourceRoute SelectLegacyAnalyticSourceRoute(
    const AvioAntialiasingConfig& config,
    bool source_enabled,
    bool compatible_paint,
    bool whole_scope_1x_proof) {
  if (!source_enabled || !compatible_paint) {
    return LegacyAnalyticSourceRoute::kGeometry;
  }
  if (!config.UsesCoverage()) {
    return LegacyAnalyticSourceRoute::kLegacyPass;
  }
  if (whole_scope_1x_proof && config.continuous_requested_classes == 0) {
    return LegacyAnalyticSourceRoute::kCoverageDirect1x;
  }
  return LegacyAnalyticSourceRoute::kCoverageNative4;
}

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_DISPLAY_LIST_LEGACY_ANALYTIC_SOURCE_H_
