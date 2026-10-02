// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <limits>
#include "gtest/gtest.h"
#include "impeller/display_list/coverage_classifier.h"

namespace impeller {
namespace {
Rect Bounds() {
  return Rect::MakeLTRB(0, 0, 100, 100);
}
CoverageDrawFacts UniformRect(Rect bounds) {
  return {.kind = CoverageGeometryKind::kRect,
          .physical_bounds = bounds,
          .uniform_source = true,
          .source_is_opaque = true,
          .uniform_geometry = CoverageConvexQuad4::Make(bounds.GetPoints())};
}
TEST(CoverageClassifier,
     NestedLayersCountSimultaneousDemandAndRetireOnRestore) {
  CoverageDisplayListPlan plan(Bounds(), 4);
  plan.SaveLayer(Rect::MakeLTRB(0, 0, 20, 20), false, false);
  plan.Save();
  plan.SaveLayer(Rect::MakeLTRB(0, 0, 10, 10), false, false);
  EXPECT_EQ(plan.GetPeakLayerBytes(), 2000u);
  plan.Restore();
  plan.Restore();
  plan.Restore();
  plan.SaveLayer(Rect::MakeLTRB(0, 0, 30, 20), false, false);
  EXPECT_EQ(plan.GetPeakLayerBytes(), 2400u);
  EXPECT_EQ(plan.GetScopes()[1].peak_live_layer_bytes, 2000u);
  EXPECT_EQ(plan.GetScopes()[2].nominal_layer_bytes, 400u);
  EXPECT_EQ(plan.GetScopes()[3].peak_live_layer_bytes, 2400u);
}
TEST(CoverageClassifier, ElidedLayerHasFactsWithoutInventingAllocationDemand) {
  CoverageDisplayListPlan plan(Bounds(), 8);
  plan.SaveLayer(Bounds(), true, false);
  plan.RecordDraw(
      {.kind = CoverageGeometryKind::kArc, .physical_bounds = Bounds()});
  EXPECT_EQ(plan.GetPeakLayerBytes(), 0u);
  EXPECT_TRUE(plan.GetScopes()[1].layer_is_elided);
  EXPECT_EQ(plan.GetScopes()[1].coverage_draw_count, 1u);
  EXPECT_EQ(plan.GetScopes()[1]
                .reasons[static_cast<size_t>(AvioCoverageReason::kClassArc)]
                .draw_count,
            1u);
  EXPECT_EQ(plan.GetRoot()
                .reasons[static_cast<size_t>(AvioCoverageReason::kClassArc)]
                .draw_count,
            1u);
}
TEST(CoverageClassifier, ClipFringeUsesActualDrawCoverageAndBlend) {
  CoverageDisplayListPlan plan(Bounds(), 4);
  const Rect clip = Rect::MakeLTRB(10.25, 10.25, 80.75, 80.75);
  plan.RecordClip(clip, Matrix{}, ClipOperation::kIntersect, true, true);
  plan.RecordDraw(UniformRect(Bounds()));
  EXPECT_EQ(plan.GetClips()[0].GetStrategy(), ClipCoverageStrategy::kPerDraw);
  plan.RecordDraw(UniformRect(Bounds()));
  EXPECT_EQ(plan.GetClips()[0].GetStrategy(),
            ClipCoverageStrategy::kFringeLayer);
  plan.RecordDraw({.kind = CoverageGeometryKind::kPath,
                   .physical_bounds = Bounds(),
                   .blend = BlendMode::kClear});
  EXPECT_EQ(plan.GetClips()[0].GetStrategy(),
            ClipCoverageStrategy::kFringeIsland);
}
TEST(CoverageClassifier, DestinationReadInsideClipStillCreatesBarrier) {
  CoverageDisplayListPlan plan(Bounds(), 4);
  plan.RecordClip(Rect::MakeLTRB(.25, .25, 99.75, 99.75), Matrix{},
                  ClipOperation::kIntersect, true, true);
  plan.RecordDraw({.physical_bounds = Rect::MakeLTRB(30, 30, 40, 40),
                   .reads_destination = true});
  EXPECT_EQ(plan.GetClips()[0].GetStrategy(),
            ClipCoverageStrategy::kDestinationReadBarrier);
  EXPECT_TRUE(plan.GetRoot().reads_destination);
}
TEST(CoverageClassifier, RestoredClipDoesNotConstrainFollowingImage) {
  CoverageDisplayListPlan plan(Bounds(), 4);
  plan.Save();
  plan.RecordClip(Rect::MakeLTRB(.25, .25, 99.75, 99.75), Matrix{},
                  ClipOperation::kIntersect, true, true);
  plan.Restore();
  const Rect rect = Rect::MakeLTRB(10, 10, 20, 20);
  plan.RecordImage(rect, Matrix{},
                   {.kind = CoverageGeometryKind::kImage,
                    .physical_bounds = rect,
                    .anti_alias = false,
                    .direct_1x_eligible = true});
  auto decision =
      plan.FindImageDecision(rect, Matrix{}, false, BlendMode::kSrcOver);
  ASSERT_TRUE(decision);
  // Conservative root eligibility does not forget a restored clip's earlier
  // work. A future segmented replay may establish a more local proof.
  EXPECT_FALSE(decision->fully_joint_mask);
  EXPECT_EQ(decision->route, CoverageImageDecision::Route::kNative4);
}
TEST(CoverageClassifier,
     AmbiguousImageIdentityNeverAuthorizesUnsafeDirectRoute) {
  CoverageDisplayListPlan plan(Bounds(), 4);
  const Rect rect = Rect::MakeLTRB(10, 10, 20, 20);
  const CoverageDrawFacts image{.kind = CoverageGeometryKind::kImage,
                                .physical_bounds = rect,
                                .anti_alias = false,
                                .direct_1x_eligible = true};
  plan.RecordImage(rect, Matrix{}, image);
  plan.RecordDraw(
      {.kind = CoverageGeometryKind::kPath, .physical_bounds = rect});
  plan.RecordImage(rect, Matrix{}, image);
  EXPECT_FALSE(
      plan.FindImageDecision(rect, Matrix{}, false, BlendMode::kSrcOver));
  EXPECT_FALSE(plan.FindImageDecision(rect, Matrix::MakeTranslation({1, 0, 0}),
                                      false, BlendMode::kSrcOver));
}
TEST(CoverageClassifier, EveryTypedReasonReportsClampedBoundingBoxPixels) {
  CoverageDisplayListPlan plan(Bounds(), 4);
  plan.RecordDraw(
      {.kind = CoverageGeometryKind::kPath, .physical_bounds = Bounds()});
  plan.RecordDraw({.kind = CoverageGeometryKind::kPath,
                   .physical_bounds = Bounds(),
                   .stroke = true});
  plan.RecordDraw(
      {.kind = CoverageGeometryKind::kArc, .physical_bounds = Bounds()});
  plan.RecordDraw(
      {.kind = CoverageGeometryKind::kBorder, .physical_bounds = Bounds()});
  plan.RecordDraw({.kind = CoverageGeometryKind::kEllipticalRoundRect,
                   .physical_bounds = Bounds()});
  plan.RecordDraw(
      {.kind = CoverageGeometryKind::kImage, .physical_bounds = Bounds()});
  plan.RecordDraw({.physical_bounds = Bounds(),
                   .anti_alias = false,
                   .analytic_source_supported = false,
                   .blend = BlendMode::kClear});
  plan.RecordDraw({.physical_bounds = Rect::MakeLTRB(-100, -100, 200, 200),
                   .analytic_source_supported = false,
                   .reads_destination = true});
  for (const auto reason :
       {AvioCoverageReason::kClassPathFill,
        AvioCoverageReason::kClassPathStroke, AvioCoverageReason::kClassArc,
        AvioCoverageReason::kClassBorder,
        AvioCoverageReason::kClassEllipticalRoundRect,
        AvioCoverageReason::kClassImageEdge,
        AvioCoverageReason::kClassNoAntialias,
        AvioCoverageReason::kClassRejectedBlend,
        AvioCoverageReason::kClassRejectedSource,
        AvioCoverageReason::kClassDestinationRead}) {
    const auto& usage = plan.GetReasons()[static_cast<size_t>(reason)];
    EXPECT_EQ(usage.draw_count, 1u);
    EXPECT_EQ(usage.pixel_area, 10000u);
  }
}
TEST(CoverageClassifier, ZeroCoverageCountDoesNotAuthorizeAliasedGeometry) {
  CoverageDisplayListPlan plan(Bounds(), 4);
  plan.RecordDraw({.kind = CoverageGeometryKind::kRect,
                   .physical_bounds = Rect::MakeLTRB(.25, .25, 10.75, 10.75)});
  EXPECT_EQ(plan.GetRoot().coverage_draw_count, 0u);
  EXPECT_FALSE(plan.GetRoot().can_render_direct_1x);
}
TEST(CoverageClassifier, OnlyPositiveUnfilteredImageProofAdmitsDirectRoot) {
  CoverageDisplayListPlan plan(Bounds(), 4);
  const Rect rect = Rect::MakeLTRB(10, 10, 20, 20);
  plan.RecordImage(rect, Matrix{},
                   {.kind = CoverageGeometryKind::kImage,
                    .physical_bounds = rect,
                    .anti_alias = false,
                    .direct_1x_eligible = true});
  EXPECT_TRUE(plan.GetRoot().can_render_direct_1x);
  auto decision =
      plan.FindImageDecision(rect, Matrix{}, false, BlendMode::kSrcOver);
  ASSERT_TRUE(decision);
  EXPECT_TRUE(decision->fully_joint_mask);
  plan.RecordDraw({.physical_bounds = rect, .anti_alias = false});
  EXPECT_FALSE(plan.GetRoot().can_render_direct_1x);
}
TEST(CoverageClassifier,
     FilterScratchIsTransientAndIncludesLiveAncestorLayers) {
  CoverageDisplayListPlan plan(Bounds(), 4);
  plan.SaveLayer(Rect::MakeLTRB(0, 0, 10, 10), false, false);
  plan.RecordScratchDemand(Rect::MakeLTRB(0, 0, 20, 20), 3);
  EXPECT_EQ(plan.GetPeakLayerBytes(), 5200u);
  plan.RecordScratchDemand(Rect::MakeLTRB(0, 0, 20, 20), 3);
  EXPECT_EQ(plan.GetPeakLayerBytes(), 5200u);
  EXPECT_EQ(plan.GetRoot().peak_live_layer_bytes, 5200u);
  plan.Restore();
  plan.RecordScratchDemand(Rect::MakeLTRB(0, 0, 20, 20), 3);
  EXPECT_EQ(plan.GetPeakLayerBytes(), 5200u);
}
TEST(CoverageClassifier, FixedCapacityOverflowDisablesAllOptimizationProofs) {
  CoverageDisplayListPlan plan(Bounds(), 4);
  const Rect rect = Rect::MakeLTRB(10, 10, 20, 20);
  for (size_t i = 0; i < 1025; i++) {
    plan.RecordImage(rect, Matrix{},
                     {.kind = CoverageGeometryKind::kImage,
                      .physical_bounds = rect,
                      .anti_alias = false,
                      .direct_1x_eligible = true});
  }
  EXPECT_TRUE(plan.HasOverflow());
  EXPECT_FALSE(plan.GetRoot().can_render_direct_1x);
  EXPECT_FALSE(
      plan.FindImageDecision(rect, Matrix{}, false, BlendMode::kSrcOver));
  plan.Reset(Bounds(), 4);
  EXPECT_FALSE(plan.HasOverflow());
  EXPECT_TRUE(plan.GetRoot().can_render_direct_1x);
  EXPECT_EQ(plan.GetScopes().size(), 1u);
}
TEST(CoverageClassifier, LegacySdfProofRequiresRealOwnerPaintAndTransform) {
  CoverageDrawFacts shape{.kind = CoverageGeometryKind::kRect,
                          .physical_bounds = Bounds(),
                          .uniform_source = true};
  const auto rotated = Matrix::MakeRotationZ(Degrees(15));
  EXPECT_TRUE(CanRenderLegacySdfAt1x(shape, rotated, true));
  EXPECT_FALSE(CanRenderLegacySdfAt1x(shape, rotated, false));
  EXPECT_FALSE(
      CanRenderLegacySdfAt1x(shape, Matrix::MakeScale({0, 1, 1}), true));
  Matrix perspective;
  perspective.m[3] = .01f;
  EXPECT_FALSE(CanRenderLegacySdfAt1x(shape, perspective, true));
  Matrix nonfinite;
  nonfinite.m[0] = std::numeric_limits<float>::infinity();
  EXPECT_FALSE(CanRenderLegacySdfAt1x(shape, nonfinite, true));
  for (const auto blend : {BlendMode::kSrc, BlendMode::kClear,
                           BlendMode::kDstIn, BlendMode::kScreen}) {
    shape.blend = blend;
    EXPECT_FALSE(CanRenderLegacySdfAt1x(shape, Matrix{}, true));
  }
  shape.blend = BlendMode::kSrcOver;
  shape.stroke = true;
  EXPECT_FALSE(CanRenderLegacySdfAt1x(shape, Matrix{}, true));
  shape.stroke = false;
  shape.anti_alias = false;
  EXPECT_FALSE(CanRenderLegacySdfAt1x(shape, Matrix{}, true));
  shape.anti_alias = true;
  shape.uniform_source = false;
  EXPECT_FALSE(CanRenderLegacySdfAt1x(shape, Matrix{}, true));
  shape.uniform_source = true;
  shape.analytic_source_supported = false;
  EXPECT_FALSE(CanRenderLegacySdfAt1x(shape, Matrix{}, true));
  shape.analytic_source_supported = true;
  shape.kind = CoverageGeometryKind::kArc;
  EXPECT_FALSE(CanRenderLegacySdfAt1x(shape, Matrix{}, true));
  shape.kind = CoverageGeometryKind::kRect;
  shape.reads_destination = true;
  EXPECT_FALSE(CanRenderLegacySdfAt1x(shape, Matrix{}, true));
}
TEST(CoverageClassifier, LegacySdfRootProofCannotSurviveMixedWorkOrClip) {
  CoverageDisplayListPlan plan(Bounds(), 4);
  const CoverageDrawFacts sdf{.kind = CoverageGeometryKind::kRoundRect,
                              .physical_bounds = Bounds(),
                              .uniform_source = true,
                              .direct_1x_eligible = true,
                              .uses_legacy_sdf = true};
  plan.RecordDraw(sdf);
  plan.RecordDraw(sdf);
  EXPECT_TRUE(plan.GetRoot().can_render_direct_1x);
  EXPECT_TRUE(plan.GetRoot().requires_legacy_sdf);
  plan.RecordDraw(
      {.kind = CoverageGeometryKind::kArc, .physical_bounds = Bounds()});
  EXPECT_FALSE(plan.GetRoot().can_render_direct_1x);
  plan.Reset(Bounds(), 4);
  plan.RecordClip(Bounds(), Matrix{}, ClipOperation::kIntersect, true, false);
  plan.RecordDraw(sdf);
  EXPECT_FALSE(plan.GetRoot().can_render_direct_1x);
  plan.Reset(Bounds(), 4);
  EXPECT_FALSE(plan.GetRoot().requires_legacy_sdf);
  plan.RecordImage(Bounds(), Matrix{},
                   {.kind = CoverageGeometryKind::kImage,
                    .physical_bounds = Bounds(),
                    .anti_alias = false,
                    .direct_1x_eligible = true});
  EXPECT_TRUE(plan.GetRoot().can_render_direct_1x);
  EXPECT_FALSE(plan.GetRoot().requires_legacy_sdf);
}
TEST(CoverageClassifier, RotatedBoundingBoxNeverProvesFullFringeGeometry) {
  CoverageDisplayListPlan plan(Bounds(), 4);
  const Rect clip = Rect::MakeLTRB(10.25, 10.25, 89.75, 89.75);
  plan.RecordClip(clip, Matrix{}, ClipOperation::kIntersect, true, true);
  const Quad diamond = {Point{50, -25}, Point{125, 50}, Point{-25, 50},
                        Point{50, 125}};
  auto draw = UniformRect(Rect::MakeLTRB(-25, -25, 125, 125));
  draw.uniform_geometry = CoverageConvexQuad4::Make(diamond);
  ASSERT_TRUE(draw.uniform_geometry);
  ASSERT_TRUE(draw.uniform_geometry->GetBounds().Contains(clip.Expand(1)));
  plan.RecordDraw(draw);
  plan.Finalize();
  EXPECT_EQ(plan.GetClips()[0].GetStrategy(),
            ClipCoverageStrategy::kFringeIsland);
  EXPECT_FALSE(plan.GetClips()[0].all_draws_full_clip_geometry);
  EXPECT_FALSE(plan.GetClips()[0].HasCertifiedSegment());
}
TEST(CoverageClassifier, SegmentCertificationCoversEarlierAndLaterFringes) {
  CoverageDisplayListPlan plan(Bounds(), 4);
  const Rect clip = Rect::MakeLTRB(10.25, 10.25, 89.75, 89.75);
  plan.RecordDraw({.kind = CoverageGeometryKind::kPath,
                   .physical_bounds = Rect::MakeLTRB(0, 0, 20, 20)});
  plan.Save();
  plan.RecordClip(clip, Matrix{}, ClipOperation::kIntersect, true, true);
  plan.RecordDraw(UniformRect(Bounds()));
  plan.Restore();
  plan.Finalize();
  EXPECT_TRUE(plan.GetClips()[0].all_draws_full_clip_geometry);
  EXPECT_FALSE(plan.GetClips()[0].no_external_fringe_correlation);
  plan.Reset(Bounds(), 4);
  plan.Save();
  plan.RecordClip(clip, Matrix{}, ClipOperation::kIntersect, true, true);
  plan.RecordDraw(UniformRect(Bounds()));
  plan.Restore();
  plan.RecordDraw({.kind = CoverageGeometryKind::kPath,
                   .physical_bounds = Rect::MakeLTRB(80, 80, 100, 100)});
  plan.Finalize();
  EXPECT_FALSE(plan.GetClips()[0].no_external_fringe_correlation);
  plan.Reset(Bounds(), 4);
  plan.RecordDraw({.kind = CoverageGeometryKind::kPath,
                   .physical_bounds = Rect::MakeLTRB(30, 30, 40, 40)});
  plan.Save();
  plan.RecordClip(clip, Matrix{}, ClipOperation::kIntersect, true, true);
  plan.RecordDraw(UniformRect(Bounds()));
  plan.RecordDraw(UniformRect(Bounds()));
  plan.Restore();
  plan.RecordDraw({.kind = CoverageGeometryKind::kPath,
                   .physical_bounds = Rect::MakeLTRB(40, 40, 50, 50)});
  plan.Finalize();
  EXPECT_TRUE(plan.GetClips()[0].HasCertifiedSegment());
  EXPECT_TRUE(plan.GetClips()[0].uniform_full_segment);
  EXPECT_EQ(plan.GetClips()[0].GetStrategy(),
            ClipCoverageStrategy::kFringeLayer);
}
TEST(CoverageClassifier, SdfCandidateNeedsActualWholeScopeSampleProof) {
  const Rect clip = Rect::MakeLTRB(10.25, 10.25, 89.75, 89.75);
  auto candidate = UniformRect(clip);
  candidate.direct_1x_eligible = true;
  candidate.uses_legacy_sdf = true;
  EXPECT_FALSE(candidate.sample_values_are_equal);
  for (const bool before_clip : {true, false}) {
    CoverageDisplayListPlan plan(Bounds(), 4);
    if (before_clip) {
      plan.RecordDraw(candidate);
    }
    plan.Save();
    plan.RecordClip(clip, Matrix{}, ClipOperation::kIntersect, true, true);
    plan.RecordDraw(UniformRect(Bounds()));
    plan.Restore();
    if (!before_clip) {
      plan.RecordDraw(candidate);
    }
    plan.Finalize();
    EXPECT_TRUE(plan.GetRoot().requires_legacy_sdf);
    EXPECT_FALSE(plan.GetRoot().can_render_direct_1x);
    ASSERT_EQ(plan.GetClips().size(), 1u);
    EXPECT_TRUE(plan.GetClips()[0].all_draws_full_clip_geometry);
    EXPECT_FALSE(plan.GetClips()[0].no_external_fringe_correlation);
    EXPECT_FALSE(plan.GetClips()[0].HasCertifiedSegment());
  }
  // Candidate eligibility remains valid when the entire actual scope selects
  // the SDF route. It is not itself a per-draw native sample equality proof.
  CoverageDisplayListPlan sdf_scope(Bounds(), 4);
  sdf_scope.RecordDraw(candidate);
  sdf_scope.Finalize();
  EXPECT_TRUE(sdf_scope.GetRoot().can_render_direct_1x);
  EXPECT_TRUE(sdf_scope.GetRoot().requires_legacy_sdf);
}
TEST(CoverageClassifier, SegmentProofIncludesEveryInteriorDrawAndClipStack) {
  CoverageDisplayListPlan plan(Bounds(), 4);
  const Rect clip = Rect::MakeLTRB(10.25, 10.25, 89.75, 89.75);
  plan.Save();
  plan.RecordClip(clip, Matrix{}, ClipOperation::kIntersect, true, true);
  plan.RecordDraw(UniformRect(Bounds()));
  plan.RecordDraw({.kind = CoverageGeometryKind::kPath,
                   .physical_bounds = Rect::MakeLTRB(30, 30, 40, 40)});
  plan.Restore();
  plan.Finalize();
  EXPECT_EQ(plan.GetClips()[0].GetStrategy(), ClipCoverageStrategy::kPerDraw);
  EXPECT_FALSE(plan.GetClips()[0].HasCertifiedSegment());
  plan.Reset(Bounds(), 4);
  plan.RecordClip(clip, Matrix{}, ClipOperation::kIntersect, true, true);
  plan.Save();
  const Rect inner = Rect::MakeLTRB(20.25, 20.25, 79.75, 79.75);
  plan.RecordClip(inner, Matrix{}, ClipOperation::kIntersect, true, true);
  plan.RecordDraw(UniformRect(Bounds()));
  plan.Restore();
  plan.Finalize();
  EXPECT_FALSE(plan.GetClips()[0].all_draws_full_clip_geometry);
  EXPECT_FALSE(plan.FindClipDecision(inner, Matrix{}, ClipOperation::kIntersect,
                                     true, true));
  const std::array<uint64_t, 1> parents = {plan.GetClips()[0].segment_token};
  const auto decision = plan.FindClipDecision(
      inner, Matrix{}, ClipOperation::kIntersect, true, true, parents);
  ASSERT_TRUE(decision);
  EXPECT_NE(decision->segment_token, parents[0]);
  EXPECT_EQ(decision->stack_tokens[0], parents[0]);
  EXPECT_EQ(decision->stack_depth, 2u);
}
TEST(CoverageClassifier, DuplicateDeclarationsCannotBorrowWrongSegmentProof) {
  CoverageDisplayListPlan plan(Bounds(), 4);
  const Rect clip = Rect::MakeLTRB(10.25, 10.25, 89.75, 89.75);
  for (size_t i = 0; i < 2; i++) {
    plan.Save();
    plan.RecordClip(clip, Matrix{}, ClipOperation::kIntersect, true, true);
    plan.RecordDraw(UniformRect(Bounds()));
    plan.Restore();
  }
  plan.Finalize();
  EXPECT_NE(plan.GetClips()[0].segment_token, plan.GetClips()[1].segment_token);
  EXPECT_FALSE(plan.FindClipDecision(clip, Matrix{}, ClipOperation::kIntersect,
                                     true, true));
}
TEST(CoverageClassifier, DeepClipDecisionRetainsExactWholeParentIdentity) {
  CoverageDisplayListPlan plan(Bounds(), 4);
  for (size_t i = 0; i < 5; i++) {
    const Scalar inset = 10.25 + i;
    plan.RecordClip(Rect::MakeLTRB(inset, inset, 100 - inset, 100 - inset),
                    Matrix{}, ClipOperation::kIntersect, true, true);
  }
  plan.RecordDraw(UniformRect(Bounds()));
  plan.Finalize();
  const auto& last = plan.GetClips()[4];
  std::array<uint64_t, 4> parents;
  for (size_t i = 0; i < parents.size(); i++) {
    parents[i] = plan.GetClips()[i].segment_token;
  }
  const auto exact =
      plan.FindClipDecision(last.physical_bounds, Matrix{},
                            ClipOperation::kIntersect, true, true, parents);
  ASSERT_TRUE(exact);
  EXPECT_EQ(exact->stack_depth, 5u);
  EXPECT_EQ(exact->stack_tokens[4], last.segment_token);
  parents[0]++;
  EXPECT_FALSE(plan.FindClipDecision(last.physical_bounds, Matrix{},
                                     ClipOperation::kIntersect, true, true,
                                     parents));
}
TEST(CoverageClassifier, FringeLayerOpacityIsAnExplicitSourceFact) {
  CoverageDisplayListPlan plan(Bounds(), 4);
  const Rect clip = Rect::MakeLTRB(10.25, 10.25, 89.75, 89.75);
  plan.RecordClip(clip, Matrix{}, ClipOperation::kIntersect, true, true);
  plan.RecordDraw(UniformRect(Bounds()));
  auto translucent = UniformRect(Bounds());
  translucent.source_is_opaque = false;
  plan.RecordDraw(translucent);
  plan.Finalize();
  EXPECT_TRUE(plan.GetClips()[0].HasCertifiedSegment());
  EXPECT_TRUE(plan.GetClips()[0].uniform_full_segment);
  EXPECT_FALSE(plan.GetClips()[0].all_sources_opaque);
}
TEST(CoverageClassifier, AnalyticImage1xRequiresCompleteMaskAndBothSidedProof) {
  CoverageDisplayListPlan plan(Bounds(), 4);
  const Rect image = Rect::MakeLTRB(10.25, 10.25, 40.75, 40.75);
  const CoverageDrawFacts source{.kind = CoverageGeometryKind::kImage,
                                 .physical_bounds = image,
                                 .source_is_opaque = true,
                                 .analytic_image_1x_eligible = true,
                                 .image_source_is_encoded = true};
  plan.RecordImage(image, Matrix{}, source);
  plan.Finalize();
  auto decision =
      plan.FindImageDecision(image, Matrix{}, true, BlendMode::kSrcOver);
  ASSERT_TRUE(decision);
  EXPECT_EQ(decision->route,
            CoverageImageDecision::Route::kAnalyticJointMask1x);
  EXPECT_TRUE(decision->fully_joint_mask);
  EXPECT_TRUE(decision->no_external_fringe_correlation);
  EXPECT_TRUE(plan.GetRoot().can_render_direct_1x);
  for (bool earlier : {true, false}) {
    plan.Reset(Bounds(), 4);
    const CoverageDrawFacts overlap{.kind = CoverageGeometryKind::kPath,
                                    .physical_bounds = image};
    if (earlier)
      plan.RecordDraw(overlap);
    plan.RecordImage(image, Matrix{}, source);
    if (!earlier)
      plan.RecordDraw(overlap);
    plan.Finalize();
    decision =
        plan.FindImageDecision(image, Matrix{}, true, BlendMode::kSrcOver);
    ASSERT_TRUE(decision);
    EXPECT_EQ(decision->route, CoverageImageDecision::Route::kNative4);
    EXPECT_FALSE(plan.GetRoot().can_render_direct_1x);
  }
  plan.Reset(Bounds(), 4);
  plan.RecordClip(Bounds(), Matrix{}, ClipOperation::kIntersect, true, false);
  plan.RecordImage(image, Matrix{}, source);
  plan.Finalize();
  decision = plan.FindImageDecision(image, Matrix{}, true, BlendMode::kSrcOver);
  ASSERT_TRUE(decision);
  EXPECT_EQ(decision->route, CoverageImageDecision::Route::kNative4);
}
TEST(CoverageClassifier, DisjointAnalyticImagesMayShareOneSampleScope) {
  CoverageDisplayListPlan plan(Bounds(), 4);
  const Rect first = Rect::MakeLTRB(10.25, 10.25, 20.75, 20.75);
  const Rect second = Rect::MakeLTRB(70.25, 70.25, 80.75, 80.75);
  for (const auto rect : {first, second}) {
    plan.RecordImage(rect, Matrix{},
                     {.kind = CoverageGeometryKind::kImage,
                      .physical_bounds = rect,
                      .source_is_opaque = true,
                      .analytic_image_1x_eligible = true,
                      .image_source_is_encoded = true});
  }
  plan.Finalize();
  EXPECT_TRUE(plan.GetRoot().can_render_direct_1x);
  for (const auto rect : {first, second}) {
    const auto decision =
        plan.FindImageDecision(rect, Matrix{}, true, BlendMode::kSrcOver);
    ASSERT_TRUE(decision);
    EXPECT_EQ(decision->route,
              CoverageImageDecision::Route::kAnalyticJointMask1x);
  }
  plan.Reset(Bounds(), 4);
  plan.RecordImage(
      first, Matrix{},
      {.kind = CoverageGeometryKind::kImage, .physical_bounds = first});
  plan.Finalize();
  EXPECT_FALSE(plan.GetRoot().can_render_direct_1x);
}
TEST(CoverageClassifier, TranslucentImageCannotCollapseNativeLaneQuantization) {
  CoverageDisplayListPlan plan(Bounds(), 4);
  const Rect image = Rect::MakeLTRB(10.25, 10.25, 40.75, 40.75);
  plan.RecordImage(image, Matrix{},
                   {.kind = CoverageGeometryKind::kImage,
                    .physical_bounds = image,
                    .source_is_opaque = false,
                    .analytic_image_1x_eligible = true,
                    .image_source_is_encoded = true});
  plan.Finalize();
  const auto decision =
      plan.FindImageDecision(image, Matrix{}, true, BlendMode::kSrcOver);
  ASSERT_TRUE(decision);
  EXPECT_FALSE(decision->source_is_opaque);
  EXPECT_EQ(decision->route, CoverageImageDecision::Route::kNative4);
  EXPECT_FALSE(plan.GetRoot().can_render_direct_1x);
}
TEST(CoverageClassifier, OpaqueRawImageDoesNotProveEncodedLaneQuantization) {
  CoverageDisplayListPlan plan(Bounds(), 4);
  const Rect image = Rect::MakeLTRB(10.25, 10.25, 40.75, 40.75);
  plan.RecordImage(image, Matrix{},
                   {.kind = CoverageGeometryKind::kImage,
                    .physical_bounds = image,
                    .source_is_opaque = true,
                    .analytic_image_1x_eligible = true});
  plan.Finalize();
  const auto decision =
      plan.FindImageDecision(image, Matrix{}, true, BlendMode::kSrcOver);
  ASSERT_TRUE(decision);
  EXPECT_TRUE(decision->source_is_opaque);
  EXPECT_FALSE(decision->source_is_encoded);
  EXPECT_EQ(decision->route, CoverageImageDecision::Route::kNative4);
  EXPECT_FALSE(plan.GetRoot().can_render_direct_1x);
}
}  // namespace
}  // namespace impeller
