// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/display_list/dl_builder.h"
#include "flutter/display_list/effects/dl_color_source.h"
#include "flutter/testing/testing.h"
#include "gtest/gtest.h"
#include "impeller/display_list/canvas.h"
#include "impeller/display_list/coverage_classifier.h"
#include "impeller/display_list/dl_text_impeller.h"
#include "impeller/display_list/legacy_analytic_source.h"
#include "impeller/typographer/backends/skia/text_frame_skia.h"
#include "third_party/skia/include/core/SkFont.h"
#include "txt/platform.h"

namespace impeller {
namespace {
TEST(CoverageDisplayListPrepass, RecordedOperationsReachScopeAndClipAnalysis) {
  flutter::DisplayListBuilder builder;
  builder.ClipRect(Rect::MakeLTRB(.25f, .25f, 99.75f, 99.75f),
                   flutter::DlClipOp::kIntersect, true);
  builder.Save();
  builder.Translate(10, 20);
  flutter::DlPaint paint;
  paint.setDrawStyle(flutter::DlDrawStyle::kStroke);
  paint.setStrokeWidth(2);
  builder.DrawArc(Rect::MakeLTRB(0, 0, 20, 10), 0, 90, false, paint);
  builder.Restore();
  const auto blur =
      flutter::DlImageFilter::MakeBlur(2, 2, flutter::DlTileMode::kClamp);
  builder.SaveLayer(Rect::MakeLTRB(0, 0, 30, 30), nullptr, blur.get());
  builder.DrawRect(Rect::MakeLTRB(0, 0, 30, 30), paint);
  builder.Restore();
  auto storage = std::make_shared<CoverageDisplayListPlan>(Rect{}, 4);
  auto plan = ClassifyCoverageDisplayList(builder.Build(), storage,
                                          Rect::MakeLTRB(0, 0, 100, 100), 4);
  ASSERT_TRUE(plan);
  ASSERT_EQ(plan->GetScopes().size(), 2u);
  ASSERT_EQ(plan->GetClips().size(), 1u);
  EXPECT_EQ(plan->GetClips()[0].GetStrategy(),
            ClipCoverageStrategy::kDestinationReadBarrier);
  EXPECT_EQ(
      plan->GetReasons()[static_cast<size_t>(AvioCoverageReason::kClassArc)]
          .draw_count,
      1u);
  EXPECT_GT(plan->GetPeakLayerBytes(),
            plan->GetScopes()[1].nominal_layer_bytes);
  EXPECT_FALSE(plan->GetRoot().can_render_direct_1x);
}
TEST(CoverageDisplayListPrepass,
     RetainedPlanCannotBeOverwrittenByNestedReplay) {
  flutter::DisplayListBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(1, 2, 3, 4), flutter::DlPaint{});
  auto list = builder.Build();
  auto storage = std::make_shared<CoverageDisplayListPlan>(Rect{}, 4);
  auto first = ClassifyCoverageDisplayList(list, storage,
                                           Rect::MakeLTRB(0, 0, 100, 100), 4);
  ASSERT_TRUE(first);
  const auto count = first->GetRoot().draw_count;
  EXPECT_FALSE(ClassifyCoverageDisplayList(list, storage,
                                           Rect::MakeLTRB(0, 0, 10, 10), 4));
  EXPECT_EQ(first->GetRoot().draw_count, count);
  first.reset();
  EXPECT_TRUE(ClassifyCoverageDisplayList(list, storage,
                                          Rect::MakeLTRB(0, 0, 10, 10), 4));
}
TEST(CoverageDisplayListPrepass, SdfEligibilityUsesActualOwnerAndDrawKinds) {
  flutter::DisplayListBuilder builder;
  const flutter::DlPaint paint;
  builder.Rotate(15);
  builder.DrawRect(Rect::MakeLTRB(2, 3, 40, 50), paint);
  builder.DrawOval(Rect::MakeLTRB(30, 40, 90, 95), paint);
  builder.DrawCircle({10, 20}, 4, paint);
  builder.DrawRoundRect(
      flutter::DlRoundRect::MakeRectRadius(Rect::MakeWH(40, 40), 6), paint);
  builder.DrawRoundSuperellipse(
      flutter::DlRoundSuperellipse::MakeRectRadius(Rect::MakeWH(40, 40), 6),
      paint);
  auto list = builder.Build();
  auto storage = std::make_shared<CoverageDisplayListPlan>(Rect{}, 4);
  auto plan = ClassifyCoverageDisplayList(
      list, storage, Rect::MakeLTRB(0, 0, 100, 100), 4, true);
  ASSERT_TRUE(plan);
  EXPECT_TRUE(plan->GetRoot().can_render_direct_1x);
  EXPECT_TRUE(plan->GetRoot().requires_legacy_sdf);
  EXPECT_EQ(plan->GetRoot().draw_count, 5u);
  plan.reset();
  plan = ClassifyCoverageDisplayList(list, storage,
                                     Rect::MakeLTRB(0, 0, 100, 100), 4, false);
  ASSERT_TRUE(plan);
  EXPECT_FALSE(plan->GetRoot().can_render_direct_1x);
  EXPECT_FALSE(plan->GetRoot().requires_legacy_sdf);
}

TEST(CoverageDisplayListPrepass, AsymmetricRseNeverClaimsUniformUberSdfScope) {
  flutter::DisplayListBuilder builder;
  builder.DrawRoundSuperellipse(
      flutter::DlRoundSuperellipse::MakeRectRadii(
          Rect::MakeWH(40, 40), RoundingRadii{.top_left = Size(5, 5),
                                              .top_right = Size(6, 6),
                                              .bottom_left = Size(7, 7),
                                              .bottom_right = Size(8, 8)}),
      flutter::DlPaint{});
  auto storage = std::make_shared<CoverageDisplayListPlan>(Rect{}, 4);
  auto plan = ClassifyCoverageDisplayList(builder.Build(), storage,
                                          Rect::MakeWH(100, 100), 4, true);
  ASSERT_TRUE(plan);
  EXPECT_FALSE(plan->GetRoot().can_render_direct_1x);
  EXPECT_FALSE(plan->GetRoot().requires_legacy_sdf);
}
TEST(CoverageDisplayListPrepass, MixedNativeGeometryNeverClaimsSdfOnlyPass) {
  flutter::DisplayListBuilder builder;
  const flutter::DlPaint paint;
  builder.DrawRect(Rect::MakeLTRB(2, 3, 40, 50), paint);
  builder.DrawArc(Rect::MakeLTRB(5, 6, 60, 70), 0, 90, false, paint);
  auto storage = std::make_shared<CoverageDisplayListPlan>(Rect{}, 4);
  const auto plan = ClassifyCoverageDisplayList(
      builder.Build(), storage, Rect::MakeLTRB(0, 0, 100, 100), 4, true);
  ASSERT_TRUE(plan);
  EXPECT_FALSE(plan->GetRoot().can_render_direct_1x);
  EXPECT_TRUE(plan->GetRoot().requires_legacy_sdf);
}

TEST(CoverageDisplayListPrepass, SdfCandidateOutsideClipUsesNativeFringeFacts) {
  const Rect bounds = Rect::MakeLTRB(0, 0, 100, 100);
  const Rect fringe = Rect::MakeLTRB(10.25f, 10.25f, 89.75f, 89.75f);
  for (const bool before_clip : {true, false}) {
    flutter::DisplayListBuilder builder;
    const flutter::DlPaint paint;
    if (before_clip) {
      builder.DrawRect(fringe, paint);
    }
    builder.Save();
    builder.ClipRect(fringe, flutter::DlClipOp::kIntersect, true);
    builder.DrawRect(bounds, paint);
    builder.Restore();
    if (!before_clip) {
      builder.DrawRect(fringe, paint);
    }
    auto storage = std::make_shared<CoverageDisplayListPlan>(Rect{}, 4);
    const auto plan =
        ClassifyCoverageDisplayList(builder.Build(), storage, bounds, 4, true);
    ASSERT_TRUE(plan);
    ASSERT_EQ(plan->GetClips().size(), 1u);
    // Actual Canvas selection is native4 for this clipped root. Keeping an
    // analytic source does not certify the complete sample/clip correlation.
    EXPECT_TRUE(plan->GetRoot().requires_legacy_sdf);
    EXPECT_FALSE(plan->GetRoot().can_render_direct_1x);
    EXPECT_FALSE(plan->GetClips()[0].no_external_fringe_correlation);
  }
}

TEST(CoverageDisplayListPrepass, ShellTextAndClipRetainNativeAnalyticSource) {
  auto data = flutter::testing::OpenFixtureAsSkData("Roboto-Regular.ttf");
  ASSERT_TRUE(data);
  auto manager = txt::GetDefaultFontManager();
  ASSERT_TRUE(manager);
  auto typeface = manager->makeFromData(data);
  ASSERT_TRUE(typeface);
  auto blob = SkTextBlob::MakeFromString("Settings", SkFont(typeface, 14));
  ASSERT_TRUE(blob);
  auto frame = MakeTextFrameFromTextBlobSkia(blob);
  ASSERT_TRUE(frame);
  ASSERT_FALSE(frame->GetBounds().IsEmpty());

  flutter::DisplayListBuilder builder;
  const flutter::DlPaint paint;
  builder.DrawRoundRect(flutter::DlRoundRect::MakeRectRadius(
                            Rect::MakeLTRB(4.25, 4.25, 195.75, 75.75), 8),
                        paint);
  builder.DrawText(flutter::DlTextImpeller::Make(frame), 20, 40, paint);
  builder.Save();
  builder.ClipRect(Rect::MakeLTRB(10.25, 10.25, 189.75, 69.75),
                   flutter::DlClipOp::kIntersect, true);
  builder.DrawCircle({175, 40}, 6, paint);
  builder.Restore();
  auto storage = std::make_shared<CoverageDisplayListPlan>(Rect{}, 4);
  const auto plan = ClassifyCoverageDisplayList(builder.Build(), storage,
                                                Rect::MakeWH(200, 80), 4, true);
  ASSERT_TRUE(plan);
  ASSERT_EQ(plan->GetRoot().draw_count, 3u);
  ASSERT_EQ(plan->GetClips().size(), 1u);
  EXPECT_FALSE(plan->GetRoot().can_render_direct_1x);
  EXPECT_TRUE(plan->GetRoot().requires_legacy_sdf);
  AvioAntialiasingConfig config;
  config.policy = AvioAntialiasingPolicy::kCoverage;
  EXPECT_EQ(SelectLegacyAnalyticSourceRoute(
                config, true, Canvas::IsCompatibleWithSDFRendering(Paint{}),
                plan->GetRoot().can_render_direct_1x),
            LegacyAnalyticSourceRoute::kCoverageNative4);
}

TEST(CoverageDisplayListPrepass,
     En50GradientAndStrokeKeepNativeAnalyticSource) {
  const Rect bounds = Rect::MakeWH(100, 100);
  flutter::DisplayListBuilder builder;
  const flutter::DlColor colors[] = {flutter::DlColor::kWhite(),
                                     flutter::DlColor::kBlack()};
  const float stops[] = {0, 1};
  flutter::DlPaint gradient;
  gradient.setColorSource(flutter::DlColorSource::MakeLinear(
      {0, 0}, {100, 100}, 2, colors, stops, flutter::DlTileMode::kClamp));
  builder.DrawRoundRect(flutter::DlRoundRect::MakeRectRadius(bounds, 8),
                        gradient);
  flutter::DlPaint stroke;
  stroke.setDrawStyle(flutter::DlDrawStyle::kStroke);
  stroke.setStrokeWidth(1.4);
  builder.DrawRoundRect(
      flutter::DlRoundRect::MakeRectRadius(Rect::MakeLTRB(20, 20, 80, 70), 2),
      stroke);
  builder.DrawLine({40, 80}, {60, 80}, stroke);
  auto storage = std::make_shared<CoverageDisplayListPlan>(Rect{}, 4);
  const auto plan =
      ClassifyCoverageDisplayList(builder.Build(), storage, bounds, 4, true);
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->GetRoot().draw_count, 3u);
  EXPECT_FALSE(plan->GetRoot().can_render_direct_1x);
  AvioAntialiasingConfig config;
  config.policy = AvioAntialiasingPolicy::kCoverage;
  EXPECT_EQ(SelectLegacyAnalyticSourceRoute(
                config, true, Canvas::IsCompatibleWithSDFRendering(Paint{}),
                plan->GetRoot().can_render_direct_1x),
            LegacyAnalyticSourceRoute::kCoverageNative4);
}
}  // namespace
}  // namespace impeller
