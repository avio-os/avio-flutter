// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder_external_view_embedder.h"

#include "gtest/gtest.h"

namespace flutter {
namespace testing {
namespace {

TEST(EmbedderExternalViewEmbedderTest,
     MaterialCoordinatesApplyRootSurfaceTransformExactlyOnce) {
  AvioCompositorMaterial material = {
      .id = 1u,
      .rect = DlRect::MakeXYWH(4.0f, 6.0f, 10.0f, 12.0f),
      .visible_rect = DlRect::MakeXYWH(5.0f, 8.0f, 8.0f, 7.0f),
      .recipe = AvioCompositorMaterialRecipe::kTiered,
      .tier = 2u,
      .corner_scale = 1.5f,
      .corner_radius = 18.0f,
      .corner_exponent = 2.0f,
      .corner_mask = 0x0fu,
  };

  const auto converted = ConvertAvioCompositorMaterialsToEmbedderCoordinates(
      {material}, DlMatrix::MakeScale({2.0f, 2.0f, 1.0f}), 2.0);

  ASSERT_EQ(converted.size(), 1u);
  EXPECT_EQ(converted[0].rect.left, 4.0);
  EXPECT_EQ(converted[0].rect.top, 6.0);
  EXPECT_EQ(converted[0].rect.right, 14.0);
  EXPECT_EQ(converted[0].rect.bottom, 18.0);
  EXPECT_EQ(converted[0].visible_rect.left, 5.0);
  EXPECT_EQ(converted[0].visible_rect.top, 8.0);
  EXPECT_EQ(converted[0].visible_rect.right, 13.0);
  EXPECT_EQ(converted[0].visible_rect.bottom, 15.0);
  EXPECT_EQ(converted[0].corner_scale, 1.5f);
}

TEST(EmbedderExternalViewEmbedderTest,
     MaterialCoordinatesScaleBottomEdgeLengthsButNotProgress) {
  AvioCompositorMaterial material = {
      .id = 2u,
      .rect = DlRect::MakeXYWH(4.0f, 6.0f, 400.0f, 80.0f),
      .visible_rect = DlRect::MakeXYWH(4.0f, 6.0f, 400.0f, 80.0f),
      .recipe = AvioCompositorMaterialRecipe::kTiered,
      .tier = 2u,
      .clip_kind = AvioCompositorMaterialClipKind::kBottomEdgePull,
      .clip_parameter_0 = 240.0f,
      .clip_parameter_1 = 0.8f,
      .clip_parameter_2 = 62.0f,
      .clip_parameter_3 = 8.0f,
  };

  const auto converted = ConvertAvioCompositorMaterialsToEmbedderCoordinates(
      {material}, DlMatrix::MakeScale({3.0f, 3.0f, 1.0f}), 2.0);

  ASSERT_EQ(converted.size(), 1u);
  EXPECT_EQ(converted[0].clip_kind,
            kFlutterAvioCompositorMaterialClipBottomEdgePull);
  EXPECT_EQ(converted[0].clip_parameter_0, 360.0);
  EXPECT_NEAR(converted[0].clip_parameter_1, 0.8, 0.000001);
  EXPECT_EQ(converted[0].clip_parameter_2, 93.0);
  EXPECT_EQ(converted[0].clip_parameter_3, 12.0);
}

TEST(EmbedderExternalViewEmbedderTest,
     PreviewCoordinatesKeepCropAndApplyDprExactlyOnce) {
  const AvioWindowPreview preview{41, DlRect::MakeXYWH(8.25f, -10.5f, 100, 60),
                                  DlRect::MakeXYWH(8.25f, 0, 100, 49.5f), 8.f,
                                  0.5f};
  const auto converted = ConvertAvioWindowPreviewsToEmbedderCoordinates(
      {preview}, DlMatrix::MakeScale({2.f, 2.f, 1.f}), 2.0);
  ASSERT_EQ(converted.size(), 1u);
  EXPECT_EQ(converted[0].rect.left, 8.25);
  EXPECT_EQ(converted[0].rect.top, -10.5);
  EXPECT_EQ(converted[0].rect.bottom, 49.5);
  EXPECT_EQ(converted[0].clip.top, 0.0);
  EXPECT_EQ(converted[0].corner_radius, 8.0);
  EXPECT_EQ(converted[0].opacity, 0.5);
}

// Hit regions leave the engine in device pixels and reach the host in
// view-local logical pixels, divided by the DPR exactly once, in paint order.
TEST(EmbedderExternalViewEmbedderTest,
     HitRegionCoordinatesApplyDprExactlyOnceAndKeepOrderAndKind) {
  AvioHitRegionSet regions;
  regions.Add({DlRect::MakeLTRB(10, 20, 110, 45), AvioHitRegionKind::kClaim});
  regions.Add(
      {DlRect::MakeLTRB(0, 0, 2000, 1250), AvioHitRegionKind::kOutputCapture});
  std::array<FlutterAvioHitRegion, FLUTTER_AVIO_MAX_HIT_REGIONS> out{};

  ASSERT_EQ(ConvertAvioHitRegionsToEmbedderCoordinates(regions, DlMatrix(),
                                                       1.25, out),
            2u);
  EXPECT_EQ(out[0].struct_size, sizeof(FlutterAvioHitRegion));
  EXPECT_DOUBLE_EQ(out[0].rect.left, 8.0);
  EXPECT_DOUBLE_EQ(out[0].rect.top, 16.0);
  EXPECT_DOUBLE_EQ(out[0].rect.right, 88.0);
  EXPECT_DOUBLE_EQ(out[0].rect.bottom, 36.0);
  EXPECT_EQ(out[0].kind, kFlutterAvioHitRegionKindClaim);
  EXPECT_DOUBLE_EQ(out[1].rect.right, 1600.0);
  EXPECT_DOUBLE_EQ(out[1].rect.bottom, 1000.0);
  EXPECT_EQ(out[1].kind, kFlutterAvioHitRegionKindOutputCapture);

  ASSERT_EQ(
      ConvertAvioHitRegionsToEmbedderCoordinates(regions, DlMatrix(), 2.0, out),
      2u);
  EXPECT_DOUBLE_EQ(out[0].rect.left, 5.0);
  EXPECT_DOUBLE_EQ(out[0].rect.top, 10.0);
  EXPECT_DOUBLE_EQ(out[0].rect.right, 55.0);
  EXPECT_DOUBLE_EQ(out[0].rect.bottom, 22.5);

  EXPECT_EQ(ConvertAvioHitRegionsToEmbedderCoordinates(AvioHitRegionSet(),
                                                       DlMatrix(), 2.0, out),
            0u);
}

AvioCompositorMaterial MakeMaterial(uint64_t id, const DlRect& rect) {
  return AvioCompositorMaterial{
      .id = id,
      .rect = rect,
      .visible_rect = rect,
      .recipe = AvioCompositorMaterialRecipe::kTiered,
      .tier = 1u,
      .corner_scale = 1.0f,
      .corner_radius = 8.0f,
      .corner_exponent = 2.0f,
      .corner_mask = 0x0fu,
  };
}

// Whether `region` covers every pixel of `rect`.
bool RegionContains(const DlRegion& region, const DlIRect& rect) {
  const DlRegion overlap = DlRegion::MakeIntersection(region, DlRegion(rect));
  return overlap.getRects(/*deband=*/true) == std::vector<DlIRect>{rect};
}

TEST(EmbedderExternalViewEmbedderTest,
     PaintCoverageIncludesMaterialThatDrewNothing) {
  EmbedderExternalView view(DlISize(800, 600), DlMatrix());
  // A compositor material layer paints no draw ops of its own; the compositor
  // is what puts pixels in that rectangle.
  ASSERT_FALSE(view.HasEngineRenderedContents());

  const DlIRect material_rect = DlIRect::MakeLTRB(200, 100, 400, 300);
  const DlRegion coverage = PaintCoverageForFrame(
      view, {MakeMaterial(1u, DlRect::Make(material_rect))});

  EXPECT_TRUE(RegionContains(coverage, material_rect));
  EXPECT_EQ(coverage.bounds(), material_rect);
}

TEST(EmbedderExternalViewEmbedderTest,
     PaintCoverageUnionsMaterialsWithRecordedDrawOps) {
  EmbedderExternalView view(DlISize(800, 600), DlMatrix());
  const DlIRect drawn_rect = DlIRect::MakeLTRB(10, 10, 60, 60);
  view.GetCanvas()->DrawRect(DlRect::Make(drawn_rect),
                             DlPaint(DlColor::kRed()));
  ASSERT_TRUE(view.HasEngineRenderedContents());

  const DlIRect material_rect = DlIRect::MakeLTRB(200, 100, 400, 300);
  const DlRegion recorded_only = PaintCoverageForFrame(view, {});
  EXPECT_TRUE(RegionContains(recorded_only, drawn_rect));
  EXPECT_FALSE(recorded_only.intersects(material_rect));

  const DlRegion coverage = PaintCoverageForFrame(
      view, {MakeMaterial(1u, DlRect::Make(material_rect))});
  EXPECT_TRUE(RegionContains(coverage, drawn_rect));
  EXPECT_TRUE(RegionContains(coverage, material_rect));
}

TEST(EmbedderExternalViewEmbedderTest,
     RootTargetAcquisitionCarriesExactOpportunityIdentity) {
  FlutterFrameOpportunityId observed_opportunity = 0u;
  FlutterEngineDisplayId observed_display = 0u;
  FlutterViewId observed_view = -1;
  EmbedderExternalViewEmbedder embedder(
      kFlutterCompositorModeRootRenderTarget,
      /*selected_target_damage=*/true,
      /*avoid_backing_store_cache=*/true,
      /*create_render_target_callback=*/nullptr,
      [&](GrDirectContext*, const std::shared_ptr<impeller::AiksContext>&,
          const FlutterBackingStoreConfig& config,
          FlutterFrameOpportunityId opportunity_id,
          FlutterEngineDisplayId display_id) {
        observed_opportunity = opportunity_id;
        observed_display = display_id;
        observed_view = config.view_id;
        return EmbedderExternalViewEmbedder::RenderTargetAcquisition{
            .status =
                ExternalViewEmbedder::RootRenderTargetAcquisition::kWithdrawn,
            .target = nullptr,
        };
      },
      /*present_callback=*/nullptr,
      [](FlutterViewId, FlutterFrameOpportunityId, FlutterEngineDisplayId,
         FlutterPresentRenderTargetStatus, const FlutterBackingStore*,
         const FlutterBackingStorePresentInfo*,
         const std::vector<FlutterAvioCompositorMaterial>&, bool,
         const std::vector<FlutterAvioWindowPreview>&, bool,
         const AvioFrameFacts&, const FlutterAvioHitRegion*,
         size_t) { return true; });
  ExternalViewEmbedder& boundary = embedder;
  boundary.BeginFrame(nullptr, nullptr);
  boundary.SetFrameOpportunity(FrameOpportunityContext{
      .id = 73u,
      .display_id = 11,
      .target_ids = {29},
  });
  boundary.PrepareFlutterView(DlISize(800, 600), 1.0);
  const auto target_info =
      boundary.AcquireRootRenderTarget(29, nullptr, nullptr);

  ASSERT_TRUE(target_info.has_value());
  EXPECT_EQ(observed_opportunity, 73u);
  EXPECT_EQ(observed_display, 11u);
  EXPECT_EQ(observed_view, 29);
  EXPECT_EQ(boundary.GetRootRenderTargetAcquisition(29),
            ExternalViewEmbedder::RootRenderTargetAcquisition::kWithdrawn);
}

TEST(EmbedderExternalViewEmbedderTest,
     EmptyRootReportsOneExactFrameWithoutTargetAndTransitionsNormally) {
  size_t acquisitions = 0, presentations = 0;
  bool accept = true;
  std::vector<FlutterPresentRenderTargetStatus> statuses;
  EmbedderExternalViewEmbedder embedder(
      kFlutterCompositorModeRootRenderTarget, true, false, nullptr,
      [&](GrDirectContext*, const std::shared_ptr<impeller::AiksContext>&,
          const FlutterBackingStoreConfig& config, FlutterFrameOpportunityId id,
          FlutterEngineDisplayId display) {
        ++acquisitions;
        EXPECT_EQ(config.view_id, 29);
        EXPECT_EQ(id, 74u);
        EXPECT_EQ(display, 11u);
        return EmbedderExternalViewEmbedder::RenderTargetAcquisition{
            .status =
                ExternalViewEmbedder::RootRenderTargetAcquisition::kWithdrawn,
            .target = nullptr};
      },
      nullptr,
      [&](FlutterViewId view, FlutterFrameOpportunityId id,
          FlutterEngineDisplayId display,
          FlutterPresentRenderTargetStatus status,
          const FlutterBackingStore* target,
          const FlutterBackingStorePresentInfo* damage,
          const std::vector<FlutterAvioCompositorMaterial>& materials,
          bool invalid_materials,
          const std::vector<FlutterAvioWindowPreview>& previews,
          bool invalid_previews, const AvioFrameFacts& facts,
          const FlutterAvioHitRegion* hit_regions, size_t hit_regions_count) {
        ++presentations;
        statuses.push_back(status);
        EXPECT_EQ(view, 29);
        EXPECT_EQ(id, 73u);
        EXPECT_EQ(display, 11u);
        EXPECT_EQ(status, kFlutterPresentRenderTargetStatusEmptyContent);
        EXPECT_EQ(target, nullptr);
        EXPECT_EQ(damage, nullptr);
        EXPECT_TRUE(materials.empty() && previews.empty());
        EXPECT_FALSE(invalid_materials || invalid_previews);
        EXPECT_EQ(facts.item_opacity, 0.25);
        EXPECT_TRUE(facts.ground_authored);
        // A revision that authored no claim claims nothing.
        EXPECT_EQ(hit_regions, nullptr);
        EXPECT_EQ(hit_regions_count, 0u);
        return accept;
      },
      kFlutterAvioExtensionFeatureEmptyFrame |
          kFlutterAvioExtensionFeatureItemEffects |
          kFlutterAvioExtensionFeatureOutputGround);
  ExternalViewEmbedder& boundary = embedder;
  boundary.BeginFrame(nullptr, nullptr);
  boundary.SetFrameOpportunity(
      FrameOpportunityContext{.id = 73u, .display_id = 11, .target_ids = {29}});
  boundary.PrepareFlutterView(DlISize(800, 600), 1.0);
  SurfaceFrame::SubmitInfo info;
  info.avio_frame_facts.item_effect_declaration_id = 1;
  info.avio_frame_facts.item_opacity = 0.25;
  info.avio_frame_facts.ground_authored = true;
  ASSERT_TRUE(boundary.SubmitAvioEmptyFrame(29, info));
  EXPECT_EQ(acquisitions, 0u);
  EXPECT_EQ(presentations, 1u);
  EXPECT_FALSE(boundary.SubmitAvioEmptyFrame(29, info));
  EXPECT_EQ(presentations, 1u);
  EXPECT_EQ(boundary.GetRootRenderTargetResult(29),
            ExternalViewEmbedder::RootRenderTargetResult::kPresented);
  // The next opportunity must use the ordinary typed acquisition path.
  boundary.BeginFrame(nullptr, nullptr);
  boundary.SetFrameOpportunity(
      FrameOpportunityContext{.id = 74u, .display_id = 11, .target_ids = {29}});
  boundary.PrepareFlutterView(DlISize(800, 600), 1.0);
  boundary.AcquireRootRenderTarget(29, nullptr, nullptr);
  EXPECT_EQ(acquisitions, 1u);
  EXPECT_EQ(presentations, 1u);
}

TEST(EmbedderExternalViewEmbedderTest,
     EmptyFrameRefusesProceduralSidecarsAndUnnegotiatedFactsBeforeAcquisition) {
  size_t acquisitions = 0, presentations = 0;
  EmbedderExternalViewEmbedder embedder(
      kFlutterCompositorModeRootRenderTarget, true, true, nullptr,
      [&](GrDirectContext*, const std::shared_ptr<impeller::AiksContext>&,
          const FlutterBackingStoreConfig&, FlutterFrameOpportunityId,
          FlutterEngineDisplayId) {
        ++acquisitions;
        return EmbedderExternalViewEmbedder::RenderTargetAcquisition{};
      },
      nullptr,
      [&](FlutterViewId, FlutterFrameOpportunityId, FlutterEngineDisplayId,
          FlutterPresentRenderTargetStatus status,
          const FlutterBackingStore* target,
          const FlutterBackingStorePresentInfo*,
          const std::vector<FlutterAvioCompositorMaterial>&, bool,
          const std::vector<FlutterAvioWindowPreview>&, bool,
          const AvioFrameFacts& facts, const FlutterAvioHitRegion* hit_regions,
          size_t hit_regions_count) {
        ++presentations;
        EXPECT_EQ(status, kFlutterPresentRenderTargetStatusInvalidFrameFacts);
        EXPECT_EQ(hit_regions, nullptr);
        EXPECT_EQ(hit_regions_count, 0u);
        EXPECT_EQ(target, nullptr);
        EXPECT_FALSE(facts.HasMetadata());
        return false;
      },
      kFlutterAvioExtensionFeatureEmptyFrame);
  ExternalViewEmbedder& boundary = embedder;
  for (size_t scenario = 0; scenario < 4; ++scenario) {
    boundary.BeginFrame(nullptr, nullptr);
    boundary.SetFrameOpportunity(FrameOpportunityContext{
        .id = scenario + 1u, .display_id = 11, .target_ids = {29}});
    boundary.PrepareFlutterView(DlISize(800, 600), 1.0);
    SurfaceFrame::SubmitInfo info;
    if (scenario == 0) {
      info.avio_frame_facts.item_effect_declaration_id = 1;
      info.avio_frame_facts.item_opacity = 0.5;
    }
    if (scenario == 1)
      info.avio_compositor_materials.push_back(
          MakeMaterial(1u, DlRect::MakeXYWH(0, 0, 10, 10)));
    if (scenario == 2)
      info.avio_window_previews_invalid = true;
    if (scenario == 3)
      info.avio_frame_facts.ready_content_revision = 9u;
    EXPECT_FALSE(boundary.SubmitAvioEmptyFrame(29, info));
  }
  EXPECT_EQ(acquisitions, 0u);
  EXPECT_EQ(presentations, 4u);
}

using PresentedHitRegions = std::vector<FlutterAvioHitRegion>;

// A root-target embedder that records every terminal it reports.
struct RecordingRootTarget {
  explicit RecordingRootTarget(FlutterAvioExtensionFeatures features)
      : embedder(
            kFlutterCompositorModeRootRenderTarget,
            /*selected_target_damage=*/true,
            /*avoid_backing_store_cache=*/true,
            /*create_render_target_callback=*/nullptr,
            [this](GrDirectContext*,
                   const std::shared_ptr<impeller::AiksContext>&,
                   const FlutterBackingStoreConfig&,
                   FlutterFrameOpportunityId,
                   FlutterEngineDisplayId) {
              ++acquisitions;
              return EmbedderExternalViewEmbedder::RenderTargetAcquisition{};
            },
            /*present_callback=*/nullptr,
            [this](FlutterViewId,
                   FlutterFrameOpportunityId,
                   FlutterEngineDisplayId,
                   FlutterPresentRenderTargetStatus status,
                   const FlutterBackingStore*,
                   const FlutterBackingStorePresentInfo*,
                   const std::vector<FlutterAvioCompositorMaterial>&,
                   bool,
                   const std::vector<FlutterAvioWindowPreview>&,
                   bool,
                   const AvioFrameFacts&,
                   const FlutterAvioHitRegion* regions,
                   size_t count) {
              statuses.push_back(status);
              EXPECT_EQ(regions == nullptr, count == 0u);
              hit_regions.emplace_back(regions, regions + count);
              return true;
            },
            features) {}

  // Submits one empty revision for view 29 at |dpr|.
  bool SubmitEmpty(const AvioHitRegionSet& regions, double dpr) {
    ExternalViewEmbedder& boundary = embedder;
    boundary.BeginFrame(nullptr, nullptr);
    boundary.SetFrameOpportunity(FrameOpportunityContext{
        .id = ++opportunity, .display_id = 11, .target_ids = {29}});
    boundary.PrepareFlutterView(DlISize(2000, 1250), dpr);
    SurfaceFrame::SubmitInfo info;
    info.avio_hit_regions = regions;
    return boundary.SubmitAvioEmptyFrame(29, info);
  }

  EmbedderExternalViewEmbedder embedder;
  size_t acquisitions = 0;
  uint64_t opportunity = 0;
  std::vector<FlutterPresentRenderTargetStatus> statuses;
  std::vector<PresentedHitRegions> hit_regions;
};

constexpr auto kEmptyFrameWithHitRegions =
    kFlutterAvioExtensionFeatureEmptyFrame |
    kFlutterAvioExtensionFeatureHitRegions;

// An empty revision (a hot zone with no pixels) still carries its complete
// claim, in logical pixels, in the same callback as the EmptyContent terminal.
TEST(EmbedderExternalViewEmbedderTest,
     EmptyContentCarriesItsHitRegionsInLogicalPixels) {
  RecordingRootTarget target(kEmptyFrameWithHitRegions);
  AvioHitRegionSet regions;
  regions.Add(
      {DlRect::MakeLTRB(0, 1225, 2000, 1250), AvioHitRegionKind::kClaim});
  ASSERT_TRUE(target.SubmitEmpty(regions, 1.25));

  ASSERT_EQ(target.statuses.size(), 1u);
  EXPECT_EQ(target.statuses[0], kFlutterPresentRenderTargetStatusEmptyContent);
  ASSERT_EQ(target.hit_regions[0].size(), 1u);
  const auto& zone = target.hit_regions[0][0];
  EXPECT_EQ(zone.struct_size, sizeof(FlutterAvioHitRegion));
  EXPECT_DOUBLE_EQ(zone.rect.left, 0.0);
  EXPECT_DOUBLE_EQ(zone.rect.top, 980.0);
  EXPECT_DOUBLE_EQ(zone.rect.right, 1600.0);
  EXPECT_DOUBLE_EQ(zone.rect.bottom, 1000.0);
  EXPECT_EQ(zone.kind, kFlutterAvioHitRegionKindClaim);
  EXPECT_EQ(target.acquisitions, 0u);
}

// A claim the host did not negotiate, or a set the frame could not describe
// exactly, is refused as invalid frame facts. The refusal carries no claim.
TEST(EmbedderExternalViewEmbedderTest,
     EmptyContentRefusesUnnegotiatedOrInvalidHitRegions) {
  AvioHitRegionSet claim;
  claim.Add({DlRect::MakeLTRB(0, 0, 10, 10), AvioHitRegionKind::kClaim});
  AvioHitRegionSet invalid = claim;
  invalid.Invalidate();

  RecordingRootTarget unnegotiated(kFlutterAvioExtensionFeatureEmptyFrame);
  EXPECT_FALSE(static_cast<ExternalViewEmbedder&>(unnegotiated.embedder)
                   .SupportsAvioHitRegions());
  EXPECT_FALSE(unnegotiated.SubmitEmpty(claim, 1.0));
  // Without the feature an empty claim set is still an ordinary empty frame.
  EXPECT_TRUE(unnegotiated.SubmitEmpty(AvioHitRegionSet(), 1.0));

  RecordingRootTarget negotiated(kEmptyFrameWithHitRegions);
  EXPECT_TRUE(static_cast<ExternalViewEmbedder&>(negotiated.embedder)
                  .SupportsAvioHitRegions());
  EXPECT_FALSE(negotiated.SubmitEmpty(invalid, 1.0));

  EXPECT_EQ(unnegotiated.statuses,
            (std::vector<FlutterPresentRenderTargetStatus>{
                kFlutterPresentRenderTargetStatusInvalidFrameFacts,
                kFlutterPresentRenderTargetStatusEmptyContent}));
  EXPECT_EQ(negotiated.statuses,
            (std::vector<FlutterPresentRenderTargetStatus>{
                kFlutterPresentRenderTargetStatusInvalidFrameFacts}));
  for (const auto* target : {&unnegotiated, &negotiated}) {
    for (const auto& presented : target->hit_regions) {
      EXPECT_TRUE(presented.empty());
    }
    EXPECT_EQ(target->acquisitions, 0u);
  }
}

}  // namespace
}  // namespace testing
}  // namespace flutter
