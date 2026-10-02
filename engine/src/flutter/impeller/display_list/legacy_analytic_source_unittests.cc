// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/display_list/legacy_analytic_source.h"

#include "gtest/gtest.h"
#include "impeller/display_list/coverage_classifier.h"

namespace impeller {
namespace {

TEST(LegacyAnalyticSource, MixedAndClippedCoverageKeepsOriginalSource) {
  AvioAntialiasingConfig config;
  config.policy = AvioAntialiasingPolicy::kCoverage;
  EXPECT_EQ(SelectLegacyAnalyticSourceRoute(config, true, true, false),
            LegacyAnalyticSourceRoute::kCoverageNative4);
}

TEST(LegacyAnalyticSource, OnlyWholeScopeProofSelectsDirect1x) {
  AvioAntialiasingConfig config;
  config.policy = AvioAntialiasingPolicy::kCoverage;
  EXPECT_EQ(SelectLegacyAnalyticSourceRoute(config, true, true, true),
            LegacyAnalyticSourceRoute::kCoverageDirect1x);
  config.continuous_requested_classes =
      static_cast<uint64_t>(AvioContinuousClass::kRectClip);
  EXPECT_EQ(SelectLegacyAnalyticSourceRoute(config, true, true, true),
            LegacyAnalyticSourceRoute::kCoverageNative4);
}

TEST(LegacyAnalyticSource, OwnerAndPaintCompatibilityRemainAuthoritative) {
  AvioAntialiasingConfig config;
  for (auto policy :
       {AvioAntialiasingPolicy::kMsaa4, AvioAntialiasingPolicy::kCoverage}) {
    config.policy = policy;
    EXPECT_EQ(SelectLegacyAnalyticSourceRoute(config, false, true, true),
              LegacyAnalyticSourceRoute::kGeometry);
    EXPECT_EQ(SelectLegacyAnalyticSourceRoute(config, true, false, true),
              LegacyAnalyticSourceRoute::kGeometry);
  }
}

TEST(LegacyAnalyticSource, LegacyPassDoesNotAcquireCoveragePolicySemantics) {
  AvioAntialiasingConfig config;
  EXPECT_EQ(SelectLegacyAnalyticSourceRoute(config, true, true, false),
            LegacyAnalyticSourceRoute::kLegacyPass);
  EXPECT_EQ(SelectLegacyAnalyticSourceRoute(config, true, true, true),
            LegacyAnalyticSourceRoute::kLegacyPass);
}

TEST(LegacyAnalyticSource, ActualScopeProofChangesTargetWithoutLosingSource) {
  AvioAntialiasingConfig config;
  config.policy = AvioAntialiasingPolicy::kCoverage;
  const Rect bounds = Rect::MakeWH(200, 80);
  CoverageDisplayListPlan plan(bounds, 4);
  const CoverageDrawFacts sdf{.kind = CoverageGeometryKind::kRoundRect,
                              .physical_bounds = bounds,
                              .uniform_source = true,
                              .direct_1x_eligible = true,
                              .uses_legacy_sdf = true};
  plan.RecordDraw(sdf);
  EXPECT_EQ(SelectLegacyAnalyticSourceRoute(
                config, true, true, plan.GetRoot().can_render_direct_1x),
            LegacyAnalyticSourceRoute::kCoverageDirect1x);
  plan.RecordDraw({.kind = CoverageGeometryKind::kText,
                   .physical_bounds = Rect::MakeLTRB(20, 25, 100, 45)});
  EXPECT_EQ(SelectLegacyAnalyticSourceRoute(
                config, true, true, plan.GetRoot().can_render_direct_1x),
            LegacyAnalyticSourceRoute::kCoverageNative4);
  plan.Reset(bounds, 4);
  plan.RecordClip(bounds, Matrix{}, ClipOperation::kIntersect, true, true);
  plan.RecordDraw(sdf);
  EXPECT_EQ(SelectLegacyAnalyticSourceRoute(
                config, true, true, plan.GetRoot().can_render_direct_1x),
            LegacyAnalyticSourceRoute::kCoverageNative4);
}

TEST(LegacyAnalyticSource, StrokeAndGradientSourceDoNotNeedRoot1xProof) {
  AvioAntialiasingConfig config;
  config.policy = AvioAntialiasingPolicy::kCoverage;
  CoverageDrawFacts sdf{.kind = CoverageGeometryKind::kRoundRect,
                        .physical_bounds = Rect::MakeWH(100, 100),
                        .uniform_source = true};
  sdf.stroke = true;
  EXPECT_FALSE(CanRenderLegacySdfAt1x(sdf, Matrix{}, true));
  EXPECT_EQ(SelectLegacyAnalyticSourceRoute(config, true, true, false),
            LegacyAnalyticSourceRoute::kCoverageNative4);
  sdf.stroke = false;
  sdf.uniform_source = false;
  EXPECT_FALSE(CanRenderLegacySdfAt1x(sdf, Matrix{}, true));
  EXPECT_EQ(SelectLegacyAnalyticSourceRoute(config, true, true, false),
            LegacyAnalyticSourceRoute::kCoverageNative4);
}

}  // namespace
}  // namespace impeller
