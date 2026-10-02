// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <array>
#include <cmath>

#include "flutter/display_list/dl_builder.h"
#include "flutter/display_list/effects/dl_color_source.h"
#include "flutter/display_list/effects/dl_image_filter.h"
#include "flutter/fml/closure.h"
#include "gtest/gtest.h"
#include "impeller/golden_tests/golden_edge_comparison.h"
#include "impeller/golden_tests/golden_renderer_vk.h"
#include "impeller/typographer/backends/skia/typographer_context_skia.h"

namespace impeller::testing {
namespace {
using namespace flutter;

// Both cards name the same actual frozen prefix. The first source is a real
// immutable Gaussian snapshot, followed by low-alpha painter stores. The
// gradient crosses adjacent encoded source values, so intermediate source
// sampling cannot be treated as an already stored UNORM byte.
sk_sp<DisplayList> MakeCapturedGroup(float scale,
                                     float phase,
                                     uint8_t destination_byte,
                                     float sigma,
                                     bool aa) {
  DisplayListBuilder builder;
  DlPaint background;
  background.setColor(
      DlColor::ARGB(255, destination_byte, destination_byte, destination_byte));
  builder.DrawPaint(background);
  builder.Scale(scale, scale);
  builder.Translate(phase, phase);
  const DlColor colors[] = {
      DlColor::ARGB(255, destination_byte, destination_byte + 1,
                    destination_byte + 2),
      DlColor::ARGB(255, destination_byte + 1, destination_byte + 2,
                    destination_byte + 3)};
  const float stops[] = {0, 1};
  DlPaint source;
  source.setColorSource(DlColorSource::MakeLinear({0, 0}, {192, 0}, 2, colors,
                                                  stops, DlTileMode::kClamp));
  builder.DrawRect(DlRect::MakeXYWH(0, 0, 192, 112), source);
  const auto filter = DlImageFilter::MakeBlur(sigma, sigma, DlTileMode::kClamp);
  if (!filter) {
    return nullptr;
  }
  for (float left : {12.25f, 104.25f}) {
    const auto bounds = DlRect::MakeXYWH(left, 16.5f, 75.5f, 78.25f);
    builder.Save();
    builder.ClipRoundRect(DlRoundRect::MakeRectRadius(bounds, 16.25f),
                          DlClipOp::kIntersect, aa);
    builder.SaveLayer(bounds, nullptr, filter.get(), 17);
    DlPaint fill;
    fill.setColor(DlColor(0x01010203u)).setBlendMode(DlBlendMode::kSrcOver);
    for (size_t paint = 0; paint < 16; paint++) {
      builder.DrawRect(DlRect::MakeXYWH(left - 4, 12, 84, 88), fill);
    }
    builder.Restore();
    builder.Restore();
  }
  return builder.Build();
}

}  // namespace

// Authored native fixture: no CPU oracle substitutes for driver attachment
// rounding, real source sampling, command ordering, or fenced readback.
TEST(AvioCoverageGoldenVKStandalone,
     CapturedBackdropPainterStoresPreserveNativeClipEdges) {
  constexpr std::array policies = {AvioGoldenPolicyVK::kMsaa4,
                                   AvioGoldenPolicyVK::kCoverage,
                                   AvioGoldenPolicyVK::kAliased1};
  std::array<std::shared_ptr<Context>, 3> contexts;
  std::array<std::unique_ptr<AiksContext>, 3> renderers;
  const fml::ScopedCleanupClosure shutdown([&] {
    for (size_t i = 0; i < contexts.size(); i++) {
      renderers[i].reset();
      if (contexts[i]) {
        contexts[i]->Shutdown();
      }
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
  for (float scale : {1.0f, 1.25f}) {
    for (float phase : {0.0f, 0.25f, 0.5f, 0.75f}) {
      for (uint8_t destination_byte : {1u, 2u, 96u}) {
        // Small nonzero sigma keeps a real blur filter declaration; sigma 0
        // would be removed by the public DisplayList filter factory.
        for (float sigma : {0.01f, 3.0f}) {
          SCOPED_TRACE(scale);
          SCOPED_TRACE(phase);
          SCOPED_TRACE(static_cast<unsigned>(destination_byte));
          SCOPED_TRACE(sigma);
          const ISize size{static_cast<int>(std::ceil(192 * scale)),
                           static_cast<int>(std::ceil(112 * scale))};
          std::array<std::unique_ptr<Screenshot>, 3> images;
          for (size_t i = 0; i < policies.size(); i++) {
            auto list =
                MakeCapturedGroup(scale, phase, destination_byte, sigma,
                                  policies[i] != AvioGoldenPolicyVK::kAliased1);
            ASSERT_TRUE(list);
            auto texture = RenderGoldenDisplayListVK(*renderers[i], list, size,
                                                     policies[i]);
            ASSERT_TRUE(texture);
            images[i] = ReadGoldenTextureVK(contexts[i], texture);
            ASSERT_TRUE(images[i]);
          }
          const auto bytes = static_cast<size_t>(size.Area()) * 4u;
          std::vector<uint8_t> edges(bytes);
          const auto result = CompareGoldenEdges(
              {images[0]->GetBytes(), bytes}, {images[1]->GetBytes(), bytes},
              {images[2]->GetBytes(), bytes}, edges);
          EXPECT_TRUE(result.Passes(false))
              << "exact bytes outside native-vs-aliased edges; at most one "
                 "byte on reference edges, including low-alpha painter stores";
        }
      }
    }
  }
}

}  // namespace impeller::testing
