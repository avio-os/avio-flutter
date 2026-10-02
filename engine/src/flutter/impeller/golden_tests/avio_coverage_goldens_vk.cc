// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <array>
#include <cmath>
#include <cstring>
#include <tuple>

#include "flutter/display_list/dl_builder.h"
#include "flutter/display_list/effects/dl_color_source.h"
#include "flutter/display_list/effects/dl_image_filter.h"
#include "flutter/display_list/geometry/dl_path_builder.h"
#include "flutter/fml/closure.h"
#include "flutter/testing/testing.h"
#include "gtest/gtest.h"
#include "impeller/display_list/coverage_classifier.h"
#include "impeller/display_list/dl_dispatcher.h"
#include "impeller/display_list/dl_image_impeller.h"
#include "impeller/display_list/dl_text_impeller.h"
#include "impeller/golden_tests/golden_digest.h"
#include "impeller/golden_tests/golden_edge_comparison.h"
#include "impeller/golden_tests/golden_renderer_vk.h"
#include "impeller/renderer/render_resource_scope.h"
#include "impeller/typographer/backends/skia/typographer_context_skia.h"
#include "third_party/skia/include/core/SkFont.h"
#include "third_party/skia/include/core/SkTextBlob.h"
#include "txt/platform.h"

namespace impeller::testing {
namespace {
using namespace flutter;

enum class Scene {
  kRRect,
  kSuperellipse,
  kOval,
  kText,
  kShadow,
  kLinearGradient,
  kRadialGradient,
  kPathFill,
  kPathStroke,
  kArc,
  kBorderedRRect,
  kFractionalImage,
  kRotatedImage,
  kFractionalClip,
  kRRectClip,
  kPathClip,
  kNestedClips,
  kClearClip,
  kSrcClip,
  kScreen,
  kDstIn,
  kBackdropClip,
  kChromeGlyph,
  kChargingHalo,
  kDockIsland,
  kAppTile,
  kPreviewCutout,
  kSnapOverlay,
  kSpacesCard,
  kThumbnail,
  kAbuttingPaths,
};
constexpr const char* kNames[] = {
    "SdfRRect",
    "SdfSuperellipse",
    "SdfOval",
    "Text",
    "Shadow",
    "LinearGradient",
    "RadialGradient",
    "PathFill",
    "PathStroke",
    "ProgressArc",
    "BorderAll",
    "FractionalImage",
    "RotatedThumbnail",
    "FractionalRectClip",
    "RRectClip",
    "PathClip",
    "NestedClips",
    "ClearInsideClip",
    "SrcSuperellipse",
    "Screen",
    "DstIn",
    "BackdropInsideClip",
    "ChromeGlyph",
    "ChargingHalo",
    "DockIslandEdges",
    "AppTile",
    "DockPreviewCutout",
    "SnapOverlay",
    "SpacesCard",
    "RRectThumbnail",
    "AbuttingPaths",
};
bool RequiresIdenticalBytes(Scene scene) {
  return static_cast<int>(scene) <= static_cast<int>(Scene::kRadialGradient);
}

sk_sp<DisplayList> MakeScene(const std::shared_ptr<Context>& context,
                             Scene scene,
                             float scale,
                             float phase,
                             bool dark,
                             bool aa) {
  DisplayListBuilder b;
  DlPaint background;
  background.setColor(dark ? DlColor(0xFF15171Cu) : DlColor(0xFFEEF1F6u));
  b.DrawPaint(background);
  b.Scale(scale, scale);
  b.Translate(phase, phase);
  const auto bounds = DlRect::MakeXYWH(20.25, 24.5, 148, 136);
  const auto rrect = DlRoundRect::MakeRectRadius(bounds, 22);
  const auto rse = DlRoundSuperellipse::MakeRectRadius(bounds, 22);
  DlPaint ink;
  ink.setAntiAlias(aa).setColor(dark ? DlColor(0xFFEAF2FFu)
                                     : DlColor(0xFF253957u));
  const std::array<DlColor, 2> colors = {DlColor(0xD0246AEAu),
                                         DlColor(0x8026DCA2u)};
  const float stops[] = {0, 1};
  DlPaint gradient = ink;
  gradient.setColorSource(
      DlColorSource::MakeLinear(bounds.GetLeftTop(), bounds.GetRightBottom(), 2,
                                colors.data(), stops, DlTileMode::kClamp));
  DlPathBuilder path_builder;
  path_builder.MoveTo({28, 136});
  path_builder.LineTo({60, 30});
  path_builder.CubicCurveTo({100, 12}, {126, 182}, {161, 47});
  path_builder.LineTo({151, 157});
  path_builder.Close();
  const auto path = path_builder.TakePath();
  const auto clip = [&] { b.ClipRoundRect(rrect, DlClipOp::kIntersect, aa); };
  switch (scene) {
    case Scene::kRRect:
      b.DrawRoundRect(rrect, ink);
      break;
    case Scene::kSuperellipse:
      b.DrawRoundSuperellipse(rse, ink);
      break;
    case Scene::kOval:
      b.DrawOval(bounds, ink);
      break;
    case Scene::kText: {
      auto data = flutter::testing::OpenFixtureAsSkData("Roboto-Regular.ttf");
      auto manager = txt::GetDefaultFontManager();
      if (!data || !manager)
        return nullptr;
      auto typeface = manager->makeFromData(data);
      if (!typeface)
        return nullptr;
      auto blob =
          SkTextBlob::MakeFromString("Avio 09:41", SkFont(typeface, 25));
      if (!blob)
        return nullptr;
      b.DrawText(DlTextImpeller::MakeFromBlob(blob), 25, 96, ink);
      break;
    }
    case Scene::kShadow:
      b.DrawShadow(path, DlColor(0xA0000000u), 7, false, 1);
      break;
    case Scene::kLinearGradient:
      b.DrawRoundRect(rrect, gradient);
      break;
    case Scene::kRadialGradient:
      gradient.setColorSource(DlColorSource::MakeRadial(
          {78, 70}, 115, 2, colors.data(), stops, DlTileMode::kClamp));
      b.DrawOval(bounds, gradient);
      break;
    case Scene::kPathStroke:
      ink.setDrawStyle(DlDrawStyle::kStroke).setStrokeWidth(2.25);
      [[fallthrough]];
    case Scene::kPathFill:
      b.DrawPath(path, ink);
      break;
    case Scene::kArc:
      ink.setDrawStyle(DlDrawStyle::kStroke).setStrokeWidth(5.25);
      b.DrawArc(bounds, 12, 271, false, ink);
      break;
    case Scene::kBorderedRRect:
      b.DrawDiffRoundRect(rrect,
                          DlRoundRect::MakeRectRadius(
                              DlRect::MakeXYWH(23.25, 27.5, 142, 130), 19),
                          ink);
      break;
    case Scene::kFractionalImage:
    case Scene::kRotatedImage:
    case Scene::kThumbnail: {
      TextureDescriptor desc;
      desc.size = {16, 16};
      desc.format = PixelFormat::kR8G8B8A8UNormInt;
      desc.usage = TextureUsage::kShaderRead;
      desc.storage_mode = StorageMode::kDevicePrivate;
      auto texture = context->GetResourceAllocator()->CreateTexture(desc);
      std::array<uint8_t, 16 * 16 * 4> pixels;
      for (size_t i = 0; i < pixels.size(); i += 4) {
        const bool checker = ((i / 4 / 16) / 4 + (i / 4 % 16) / 4) % 2;
        pixels[i] = checker ? 245 : 25;
        pixels[i + 1] = checker ? 45 : 185;
        pixels[i + 2] = checker ? 115 : 230;
        pixels[i + 3] = 255;
      }
      if (!texture || !texture->SetContents(pixels.data(), pixels.size()))
        return nullptr;
      if (scene == Scene::kThumbnail)
        clip();
      if (scene == Scene::kRotatedImage) {
        b.Translate(96, 96);
        b.Rotate(17);
        b.Translate(-96, -96);
      }
      b.DrawImageRect(DlImageImpeller::Make(texture), DlRect::MakeWH(16, 16),
                      bounds, DlImageSampling::kLinear, &ink);
      break;
    }
    case Scene::kFractionalClip:
      b.ClipRect(bounds, DlClipOp::kIntersect, aa);
      b.DrawPaint(gradient);
      break;
    case Scene::kPathClip:
      b.ClipPath(path, DlClipOp::kIntersect, aa);
      b.DrawPaint(gradient);
      break;
    case Scene::kNestedClips:
      clip();
      b.ClipPath(path, DlClipOp::kIntersect, aa);
      b.ClipOval(DlRect::MakeXYWH(25, 28, 140, 130), DlClipOp::kIntersect, aa);
      b.DrawPaint(gradient);
      break;
    case Scene::kClearClip:
    case Scene::kPreviewCutout:
      clip();
      b.DrawPaint(ink);
      ink.setBlendMode(DlBlendMode::kClear);
      b.DrawCircle({67.25, 76.5}, 24.5, ink);
      break;
    case Scene::kSrcClip:
    case Scene::kAppTile:
      b.ClipRoundSuperellipse(rse, DlClipOp::kIntersect, aa);
      gradient.setBlendMode(DlBlendMode::kSrc);
      b.DrawPaint(gradient);
      break;
    case Scene::kScreen:
    case Scene::kSpacesCard:
      clip();
      b.DrawRect(DlRect::MakeXYWH(20, 24, 78.25, 137), ink);
      gradient.setBlendMode(DlBlendMode::kScreen);
      b.DrawPaint(gradient);
      break;
    case Scene::kDstIn:
      clip();
      b.DrawPaint(gradient);
      ink.setAlpha(153).setBlendMode(DlBlendMode::kDstIn);
      b.DrawOval(bounds, ink);
      break;
    case Scene::kBackdropClip:
    case Scene::kSnapOverlay: {
      b.DrawRect(DlRect::MakeXYWH(16, 15, 70, 166), gradient);
      b.ClipRoundRect(DlRoundRect::MakeRectRadius(bounds, 16),
                      DlClipOp::kIntersect, aa);
      auto filter = DlImageFilter::MakeBlur(3, 3, DlTileMode::kClamp);
      DlPaint alpha;
      alpha.setAlpha(180);
      b.SaveLayer(bounds, &alpha, filter.get());
      ink.setAlpha(44);
      b.DrawPaint(ink);
      b.Restore();
      break;
    }
    case Scene::kChromeGlyph:
      ink.setDrawStyle(DlDrawStyle::kStroke).setStrokeWidth(2);
      b.DrawLine({52, 70}, {96, 111}, ink);
      b.DrawLine({96, 111}, {140, 62}, ink);
      break;
    case Scene::kChargingHalo:
      b.SaveLayer(bounds);
      b.DrawOval(bounds, gradient);
      ink.setBlendMode(DlBlendMode::kClear)
          .setDrawStyle(DlDrawStyle::kStroke)
          .setStrokeWidth(8);
      b.DrawArc(bounds, 5, 277, false, ink);
      b.Restore();
      break;
    case Scene::kDockIsland:
      b.DrawRoundSuperellipse(rse, gradient);
      b.DrawRoundRect(
          DlRoundRect::MakeRectRadius(DlRect::MakeXYWH(40, 60, 105, 49), 24),
          ink);
      break;
    case Scene::kAbuttingPaths: {
      DlPathBuilder left, right;
      left.MoveTo({20, 25});
      left.LineTo({98.25, 25});
      left.LineTo({98.25, 160});
      left.LineTo({20, 160});
      left.Close();
      right.MoveTo({98.25, 25});
      right.LineTo({168, 25});
      right.LineTo({168, 160});
      right.LineTo({98.25, 160});
      right.Close();
      b.DrawPath(left.TakePath(), ink);
      b.DrawPath(right.TakePath(), ink);
      break;
    }
    case Scene::kRRectClip:
      clip();
      b.DrawPaint(gradient);
      break;
  }
  return b.Build();
}
}  // namespace

class AvioCoverageGoldenVK
    : public ::testing::TestWithParam<std::tuple<int, float, bool>> {};

TEST_P(AvioCoverageGoldenVK, NativeFourAgainstCoverageAndAliasedOne) {
  const auto [scene_id, scale, dark] = GetParam();
  const auto scene = static_cast<Scene>(scene_id);
  const ISize size{static_cast<int>(std::ceil(192 * scale)),
                   static_cast<int>(std::ceil(192 * scale))};
  std::array<std::shared_ptr<Context>, 3> contexts;
  std::array<std::unique_ptr<AiksContext>, 3> renderers;
  const fml::ScopedCleanupClosure shutdown([&] {
    for (size_t i = 0; i < contexts.size(); ++i) {
      renderers[i].reset();
      if (contexts[i])
        contexts[i]->Shutdown();
    }
  });
  constexpr std::array policies = {AvioGoldenPolicyVK::kMsaa4,
                                   AvioGoldenPolicyVK::kCoverage,
                                   AvioGoldenPolicyVK::kAliased1};
  for (size_t i = 0; i < policies.size(); ++i) {
    contexts[i] = MakeHeadlessGoldenContextVK(policies[i], scene_id <= 2);
    ASSERT_TRUE(contexts[i] && contexts[i]->IsValid());
    renderers[i] = std::make_unique<AiksContext>(
        contexts[i], TypographerContextSkia::Make(),
        MakeGoldenAllocatorVK(contexts[i], policies[i]));
    ASSERT_TRUE(renderers[i]->IsValid());
  }
  GoldenDigest::Instance()->AddDimension("gpu_string",
                                         contexts[0]->DescribeGpuModel());
  GoldenDigest::Instance()->AddDimension("avio_render_policies",
                                         "msaa4,coverage,aliased1");
  for (float phase : {0.0f, 0.25f, 0.5f, 0.75f}) {
    SCOPED_TRACE(phase);
    std::array<std::unique_ptr<Screenshot>, 3> images;
    for (size_t i = 0; i < policies.size(); ++i) {
      auto list = MakeScene(contexts[i], scene, scale, phase, dark, i != 2);
      ASSERT_TRUE(list) << "Required real fixture/font/texture unavailable";
      auto texture =
          RenderGoldenDisplayListVK(*renderers[i], list, size, policies[i]);
      images[i] = ReadGoldenTextureVK(contexts[i], texture);
      ASSERT_TRUE(images[i]) << "Render or fenced readback failed";
    }
    const size_t bytes = static_cast<size_t>(size.Area()) * 4;
    std::vector<uint8_t> mask(bytes);
    const auto comparison = CompareGoldenEdges(
        {images[0]->GetBytes(), bytes}, {images[1]->GetBytes(), bytes},
        {images[2]->GetBytes(), bytes}, mask);
    const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string name = std::string(test->test_suite_name()) + "_" +
                       test->name() + "_Phase" +
                       std::to_string(static_cast<int>(phase * 100));
    std::replace(name.begin(), name.end(), '/', '_');
    constexpr const char* modes[] = {"_msaa4", "_coverage", "_aliased1"};
    for (size_t i = 0; i < images.size(); ++i) {
      const auto file = name + modes[i] + ".png";
      ASSERT_TRUE(images[i]->WriteToPNG(
          WorkingDirectory::Instance()->GetFilenamePath(file)));
      GoldenDigest::Instance()->AddImage(name + modes[i], file, size.width,
                                         size.height);
    }
    auto edge = MakeGoldenScreenshotVK(size, std::move(mask));
    ASSERT_TRUE(edge &&
                edge->WriteToPNG(WorkingDirectory::Instance()->GetFilenamePath(
                    name + "_edge_mask.png")));
    RecordProperty("Phase" + std::to_string(static_cast<int>(phase * 100)) +
                       "MaxInteriorDelta",
                   comparison.max_interior_delta);
    RecordProperty("Phase" + std::to_string(static_cast<int>(phase * 100)) +
                       "MaxEdgeDelta",
                   comparison.max_edge_delta);
    RecordProperty(
        "Phase" + std::to_string(static_cast<int>(phase * 100)) + "EdgePixels",
        comparison.edge_pixels);
    EXPECT_TRUE(comparison.Passes(RequiresIdenticalBytes(scene)))
        << "outside edge mask requires exact bytes; edge delta <=1; "
           "SDF/text/shadow/gradient classes require all bytes exact";
  }
}

// Exact existing SDF shader policy, rather than an aliased-geometry substitute.
// No clip, filter, destination reader, saveLayer, or other draw class appears.
TEST(AvioCoverageGoldenVKStandalone, SdfOnlyPassIsByteIdenticalAt1x) {
  constexpr std::array policies = {AvioGoldenPolicyVK::kAliased1,
                                   AvioGoldenPolicyVK::kCoverage};
  std::array<std::shared_ptr<Context>, 2> contexts;
  std::array<std::unique_ptr<AiksContext>, 2> renderers;
  const fml::ScopedCleanupClosure shutdown([&] {
    for (size_t i = 0; i < contexts.size(); ++i) {
      renderers[i].reset();
      if (contexts[i]) {
        contexts[i]->Shutdown();
      }
    }
  });
  for (size_t i = 0; i < policies.size(); ++i) {
    contexts[i] = MakeHeadlessGoldenContextVK(policies[i], true);
    ASSERT_TRUE(contexts[i] && contexts[i]->IsValid());
    renderers[i] = std::make_unique<AiksContext>(
        contexts[i], TypographerContextSkia::Make(),
        MakeGoldenAllocatorVK(contexts[i], policies[i]));
    ASSERT_TRUE(renderers[i]->IsValid());
  }
  for (float scale : {1.0f, 1.25f, 2.0f}) {
    for (float phase : {0.0f, 0.25f, 0.5f, 0.75f}) {
      SCOPED_TRACE(scale);
      SCOPED_TRACE(phase);
      DisplayListBuilder builder;
      builder.Scale(scale, scale);
      builder.Translate(phase, phase);
      DlPaint fill;
      fill.setAntiAlias(true)
          .setBlendMode(DlBlendMode::kSrcOver)
          .setColor(DlColor(0xB04578C0u));
      builder.DrawRect(DlRect::MakeXYWH(8.25, 8.5, 58.5, 56.25), fill);
      builder.DrawOval(DlRect::MakeXYWH(96.5, 8.25, 58, 54), fill);
      builder.DrawCircle(DlPoint(37.25, 125.5), 27.25, fill);
      builder.DrawRoundRect(DlRoundRect::MakeRectRadius(
                                DlRect::MakeXYWH(96.25, 96.5, 58.5, 56.25), 12),
                            fill);
      auto list = builder.Build();
      const ISize size{static_cast<int>(std::ceil(176 * scale)),
                       static_cast<int>(std::ceil(176 * scale))};
      std::array<std::unique_ptr<Screenshot>, 2> images;
      for (size_t i = 0; i < policies.size(); ++i) {
        auto texture =
            RenderGoldenDisplayListVK(*renderers[i], list, size, policies[i]);
        ASSERT_TRUE(texture);
        images[i] = ReadGoldenTextureVK(contexts[i], texture);
        ASSERT_TRUE(images[i]);
      }
      const auto bytes = static_cast<size_t>(size.Area()) * 4u;
      EXPECT_EQ(
          std::memcmp(images[0]->GetBytes(), images[1]->GetBytes(), bytes), 0);
      const auto usage = contexts[1]->GetAvioCoverageUsageReport(false);
      EXPECT_EQ(usage.coverage_flushes, 0u);
      for (size_t i = 0; i < usage.coverage_reasons_count; ++i) {
        if (usage.coverage_reasons[i].reason_id ==
            static_cast<uint32_t>(AvioCoverageReason::kNativeColourIsland)) {
          EXPECT_EQ(usage.coverage_reasons[i].draw_count, 0u);
        }
      }
    }
  }
}

// This correctness fixture exercises actual caller-generated pipeline keys,
// independently of the cold catalogue's visitor tests. It deliberately does
// not assert all GPU allocations are zero: a fresh golden output/atlas is real
// storage. Only successful native pipeline creation during raster work must
// already be absent after the actual content-context constructor returns.
TEST(AvioCoverageGoldenVKStandalone, NoPipelineCompileOnFirstFrameAfterInit) {
  auto context =
      MakeHeadlessGoldenContextVK(AvioGoldenPolicyVK::kCoverage, true);
  ASSERT_TRUE(context && context->IsValid());
  std::unique_ptr<AiksContext> renderer;
  const fml::ScopedCleanupClosure shutdown([&] {
    renderer.reset();
    context->Shutdown();
  });
  renderer = std::make_unique<AiksContext>(
      context, TypographerContextSkia::Make(),
      MakeGoldenAllocatorVK(context, AvioGoldenPolicyVK::kCoverage));
  ASSERT_TRUE(renderer->IsValid());
  // Missing real fonts/source textures or native failures fail the fixture.
  for (auto scene : {Scene::kRRect,          Scene::kSuperellipse,
                     Scene::kOval,           Scene::kText,
                     Scene::kShadow,         Scene::kLinearGradient,
                     Scene::kRadialGradient, Scene::kPathFill,
                     Scene::kPathStroke,     Scene::kArc,
                     Scene::kBorderedRRect,  Scene::kFractionalImage,
                     Scene::kFractionalClip, Scene::kPathClip,
                     Scene::kClearClip,      Scene::kSrcClip,
                     Scene::kScreen,         Scene::kDstIn,
                     Scene::kBackdropClip,   Scene::kChargingHalo}) {
    SCOPED_TRACE(kNames[static_cast<size_t>(scene)]);
    auto list = MakeScene(context, scene, 1.25f, 0.25f, true, true);
    ASSERT_TRUE(list);
    const auto initial = context->GetAvioRenderResourceReport(true);
    ASSERT_TRUE(initial.available);
    ASSERT_NE(initial.counters_supported & kAvioCounterFirstUseCompiles, 0u);
    std::shared_ptr<Texture> output;
    {
      const AvioRasterFrameScope frame;
      output = RenderGoldenDisplayListVK(*renderer, list, {240, 240},
                                         AvioGoldenPolicyVK::kCoverage);
    }
    ASSERT_TRUE(output);
    const auto report = context->GetAvioRenderResourceReport(true);
    ASSERT_TRUE(report.available);
    EXPECT_EQ(report.first_use_compiles, 0u);
  }
}

// The source families remain original analytic shaders even when text, a
// native clip or gradients forbid the whole-root 1x proof. This fixture renders
// the real source/filter calls; the CPU catalogue is not substituted for them.
TEST(AvioCoverageGoldenVKStandalone, MixedAnalyticSourcesUseColdNativeKeys) {
  for (bool use_sdfs : {false, true}) {
    SCOPED_TRACE(use_sdfs);
    auto context =
        MakeHeadlessGoldenContextVK(AvioGoldenPolicyVK::kCoverage, use_sdfs);
    ASSERT_TRUE(context && context->IsValid());
    std::unique_ptr<AiksContext> renderer;
    const fml::ScopedCleanupClosure shutdown([&] {
      renderer.reset();
      context->Shutdown();
    });
    renderer = std::make_unique<AiksContext>(
        context, TypographerContextSkia::Make(),
        MakeGoldenAllocatorVK(context, AvioGoldenPolicyVK::kCoverage));
    ASSERT_TRUE(renderer->IsValid());
    DisplayListBuilder builder;
    DlPaint background;
    background.setColor(DlColor(0xFF152235u));
    builder.DrawPaint(background);
    builder.ClipRoundRect(
        DlRoundRect::MakeRectRadius(DlRect::MakeXYWH(5.25, 6.75, 220, 220), 17),
        DlClipOp::kIntersect, true);
    DlPaint ink;
    ink.setAntiAlias(true).setColor(DlColor(0xCEEDB65Cu));
    const auto bounds = DlRect::MakeXYWH(18.25, 22.75, 125.5, 96.5);
    builder.DrawRoundRect(DlRoundRect::MakeRectRadius(bounds, 17), ink);
    builder.DrawRoundSuperellipse(
        DlRoundSuperellipse::MakeRectRadii(bounds.Shift(16, 31),
                                           {.top_left = {8, 11},
                                            .top_right = {19, 17},
                                            .bottom_left = {13, 21},
                                            .bottom_right = {25, 16}}),
        ink);
    const std::array<DlColor, 2> colors = {DlColor(0xB84087ECu),
                                           DlColor(0xE090D5A2u)};
    const float stops[] = {0, 1};
    auto gradient = ink;
    gradient.setColorSource(
        DlColorSource::MakeLinear(bounds.GetLeftTop(), bounds.GetRightBottom(),
                                  2, colors.data(), stops, DlTileMode::kClamp));
    builder.DrawOval(bounds.Shift(21, 15), gradient);
    gradient.setDrawStyle(DlDrawStyle::kStroke).setStrokeWidth(2.25);
    gradient.setBlendMode(DlBlendMode::kScreen);
    builder.DrawRect(bounds.Shift(11, 25), gradient);
    builder.DrawLine({24.25, 30.75}, {188.5, 182.25}, gradient);
    for (auto mode :
         {DlBlendMode::kSrcOver, DlBlendMode::kSrc, DlBlendMode::kClear,
          DlBlendMode::kDstIn, DlBlendMode::kDstOut, DlBlendMode::kScreen}) {
      ink.setBlendMode(mode);
      builder.DrawCircle({168.25, 162.75}, 11.5, ink);
    }
    auto font_data =
        flutter::testing::OpenFixtureAsSkData("Roboto-Regular.ttf");
    auto manager = txt::GetDefaultFontManager();
    ASSERT_TRUE(font_data && manager);
    auto typeface = manager->makeFromData(font_data);
    ASSERT_TRUE(typeface);
    auto blob = SkTextBlob::MakeFromString("Mixed Avio", SkFont(typeface, 18));
    ASSERT_TRUE(blob);
    ink.setBlendMode(DlBlendMode::kSrcOver);
    builder.DrawText(DlTextImpeller::MakeFromBlob(blob), 34, 204, ink);
    auto list = builder.Build();
    ASSERT_TRUE(list);
    {
      auto proof = ClassifyCoverageDisplayList(
          list, renderer->GetContentContext().GetCoverageClassifierStorage(),
          Rect::MakeSize(ISize{240, 240}), 4u, use_sdfs);
      ASSERT_TRUE(proof);
      ASSERT_FALSE(proof->GetRoot().can_render_direct_1x);
    }
    const auto initial = context->GetAvioRenderResourceReport(true);
    ASSERT_TRUE(initial.available);
    ASSERT_NE(initial.counters_supported & kAvioCounterFirstUseCompiles, 0u);
    std::shared_ptr<Texture> output;
    {
      const AvioRasterFrameScope frame;
      output = RenderGoldenDisplayListVK(*renderer, list, {240, 240},
                                         AvioGoldenPolicyVK::kCoverage);
    }
    ASSERT_TRUE(output);
    EXPECT_EQ(context->GetAvioRenderResourceReport(true).first_use_compiles,
              0u);
  }
}

// These sources have no full-size background/clip/filter hiding the positive
// proof. The actual classifier and Canvas select the direct1x source factory,
// rather than this test calling the cold visitor or swapping pipeline options.
TEST(AvioCoverageGoldenVKStandalone, ProvenDirectSourcesUseColdRootFormatKeys) {
  auto context =
      MakeHeadlessGoldenContextVK(AvioGoldenPolicyVK::kCoverage, true);
  ASSERT_TRUE(context && context->IsValid());
  std::unique_ptr<AiksContext> renderer;
  const fml::ScopedCleanupClosure shutdown([&] {
    renderer.reset();
    context->Shutdown();
  });
  renderer = std::make_unique<AiksContext>(
      context, TypographerContextSkia::Make(),
      MakeGoldenAllocatorVK(context, AvioGoldenPolicyVK::kCoverage));
  ASSERT_TRUE(renderer->IsValid());
  const auto allocator =
      MakeGoldenAllocatorVK(context, AvioGoldenPolicyVK::kCoverage);
  constexpr ISize size{96, 96};
  for (auto format :
       {PixelFormat::kR8G8B8A8UNormInt, PixelFormat::kB8G8R8A8UNormInt}) {
    SCOPED_TRACE(static_cast<int>(format));
    for (bool rounded : {false, true}) {
      SCOPED_TRACE(rounded);
      DisplayListBuilder builder;
      DlPaint paint;
      paint.setAntiAlias(true).setColor(DlColor(0xB84E83A9u));
      const auto bounds = DlRect::MakeXYWH(8.25, 10.75, 56.5, 53.25);
      if (rounded) {
        builder.DrawRoundRect(DlRoundRect::MakeRectRadius(bounds, 11), paint);
      } else {
        builder.DrawRect(bounds, paint);
      }
      const auto list = builder.Build();
      ASSERT_TRUE(list);
      auto& contents = renderer->GetContentContext();
      {
        auto proof = ClassifyCoverageDisplayList(
            list, contents.GetCoverageClassifierStorage(), Rect::MakeSize(size),
            4u, context->GetFlags().use_sdfs);
        ASSERT_TRUE(proof);
        ASSERT_TRUE(proof->GetRoot().can_render_direct_1x);
        ASSERT_TRUE(proof->GetRoot().requires_legacy_sdf);
      }  // return classifier storage before the real replay borrows it
      auto target = allocator->CreateOffscreen(
          *context, size, 1, "Cold direct source fixture",
          RenderTarget::kDefaultColorAttachmentConfig, std::nullopt, nullptr,
          nullptr, format);
      ASSERT_TRUE(target.IsValid());
      ASSERT_EQ(target.GetSampleCount(), SampleCount::kCount1);
      ASSERT_FALSE(target.GetDepthAttachment());
      ASSERT_FALSE(target.GetStencilAttachment());
      const auto initial = context->GetAvioRenderResourceReport(true);
      ASSERT_TRUE(initial.available);
      ASSERT_NE(initial.counters_supported & kAvioCounterFirstUseCompiles, 0u);
      {
        const AvioRasterFrameScope frame;
        ASSERT_TRUE(RenderToTarget(contents, std::move(target), list,
                                   Rect::MakeSize(size), true, false));
      }
      const auto report = context->GetAvioRenderResourceReport(true);
      ASSERT_TRUE(report.available);
      EXPECT_EQ(report.first_use_compiles, 0u);
    }
  }
}

// Actual native execution is required by this fixture: missing Vulkan,
// shader libraries, texture readback or a failed frame is an assertion failure.
// The 16-deep case exercises streamed flattening; at the maximum depth the
// renderer may conservatively choose its complete native4 replay instead.
TEST(AvioCoverageGoldenVKStandalone,
     DeepNativeClip512MatchesLegacyLaneCorrelations) {
  constexpr std::array policies = {AvioGoldenPolicyVK::kMsaa4,
                                   AvioGoldenPolicyVK::kCoverage,
                                   AvioGoldenPolicyVK::kAliased1};
  std::array<std::shared_ptr<Context>, 3> contexts;
  std::array<std::unique_ptr<AiksContext>, 3> renderers;
  const fml::ScopedCleanupClosure shutdown([&] {
    for (size_t i = 0; i < contexts.size(); i++) {
      renderers[i].reset();
      if (contexts[i])
        contexts[i]->Shutdown();
    }
  });
  for (size_t i = 0; i < policies.size(); i++) {
    contexts[i] = MakeHeadlessGoldenContextVK(policies[i]);
    ASSERT_TRUE(contexts[i] && contexts[i]->IsValid());
    renderers[i] = std::make_unique<AiksContext>(
        contexts[i], TypographerContextSkia::Make(),
        MakeGoldenAllocatorVK(contexts[i], policies[i]));
    ASSERT_TRUE(renderers[i]->IsValid());
  }
  for (size_t depth : {16u, 512u}) {
    for (float scale : {1.0f, 1.25f, 2.0f}) {
      for (float phase : {0.0f, .25f, .5f, .75f}) {
        SCOPED_TRACE(depth);
        SCOPED_TRACE(scale);
        SCOPED_TRACE(phase);
        const ISize size{static_cast<int>(std::ceil(224 * scale)),
                         static_cast<int>(std::ceil(208 * scale))};
        std::array<std::unique_ptr<Screenshot>, 3> images;
        for (size_t policy = 0; policy < policies.size(); policy++) {
          const bool aa = policies[policy] != AvioGoldenPolicyVK::kAliased1;
          DisplayListBuilder builder;
          DlPaint background;
          background.setColor(DlColor(0xFF152A42u));
          builder.DrawPaint(background);
          builder.Scale(scale, scale);
          builder.Translate(phase, phase);
          for (size_t i = 0; i < depth; i++) {
            if (i == 4) {
              builder.ClipOval(DlRect::MakeXYWH(68.25, 65.5, 24.5, 20.25),
                               DlClipOp::kDifference, aa);
            } else if (i == depth / 2) {
              builder.ClipRoundRect(
                  DlRoundRect::MakeRectRadius(
                      DlRect::MakeXYWH(104.5, 91.25, 25.25, 32.5), 6.25),
                  DlClipOp::kDifference, aa);
            } else if (i + 1 == depth) {
              DlPathBuilder hole;
              hole.MoveTo({140.25, 110.5});
              hole.LineTo({162.5, 134.25});
              hole.LineTo({136.25, 146.5});
              hole.Close();
              builder.ClipPath(hole.TakePath(), DlClipOp::kDifference, aa);
            } else {
              // The oldest rounded corner remains stricter than every later
              // rounded corner; middle/newest differences stay disjoint.
              const float inset = i == 0 ? 0 : .125f * (i % 3);
              builder.ClipRoundRect(
                  DlRoundRect::MakeRectRadius(
                      DlRect::MakeXYWH(20.25 + inset, 24.5 + inset,
                                       180 - 2 * inset, 160 - 2 * inset),
                      i == 0 ? 26.5f : 8.25f),
                  DlClipOp::kIntersect, aa);
            }
          }
          DlPaint foreground;
          foreground.setColor(DlColor(0xFFEAD36Bu));
          builder.DrawPaint(foreground);
          auto list = builder.Build();
          ASSERT_TRUE(list);
          auto texture = RenderGoldenDisplayListVK(*renderers[policy], list,
                                                   size, policies[policy]);
          ASSERT_TRUE(texture);
          images[policy] = ReadGoldenTextureVK(contexts[policy], texture);
          ASSERT_TRUE(images[policy]);
        }
        const auto bytes = static_cast<size_t>(size.Area()) * 4u;
        std::vector<uint8_t> edges(bytes);
        const auto result =
            CompareGoldenEdges(std::span(images[0]->GetBytes(), bytes),
                               std::span(images[1]->GetBytes(), bytes),
                               std::span(images[2]->GetBytes(), bytes), edges);
        EXPECT_TRUE(result.Passes(false))
            << "all retained parents/difference holes must survive; exact "
               "interiors and at most one RGBA byte at native reference edges";
      }
    }
  }
}

std::string GoldenSceneNameVK(
    const ::testing::TestParamInfo<std::tuple<int, float, bool>>& info) {
  const auto [scene, scale, dark] = info.param;
  return std::string(kNames[scene]) + (dark ? "Dark" : "Light") + "Scale" +
         std::to_string(static_cast<int>(scale * 100));
}

INSTANTIATE_TEST_SUITE_P(
    AvioSites,
    AvioCoverageGoldenVK,
    ::testing::Combine(::testing::Range(0, static_cast<int>(std::size(kNames))),
                       ::testing::Values(1.0f, 1.25f, 2.0f),
                       ::testing::Bool()),
    GoldenSceneNameVK);

}  // namespace impeller::testing
