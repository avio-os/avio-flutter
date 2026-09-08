// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cmath>
#include <vector>

#include "display_list/dl_builder.h"
#include "display_list/geometry/dl_path_builder.h"
#include "fml/synchronization/count_down_latch.h"
#include "impeller/core/device_buffer.h"
#include "impeller/display_list/aiks_context.h"
#include "impeller/display_list/aiks_unittests.h"
#include "impeller/display_list/dl_dispatcher.h"
#include "impeller/display_list/paint.h"

namespace impeller::testing {
namespace {

using namespace flutter;

std::vector<uint8_t> ReadPixels(const std::shared_ptr<Context>& context,
                                const std::shared_ptr<Texture>& texture) {
  DeviceBufferDescriptor desc;
  desc.size = texture->GetTextureDescriptor().GetByteSizeOfBaseMipLevel();
  desc.readback = true;
  desc.storage_mode = StorageMode::kHostVisible;
  auto buffer = context->GetResourceAllocator()->CreateBuffer(desc);
  if (!buffer) {
    return {};
  }
  auto command = context->CreateCommandBuffer();
  auto blit = command->CreateBlitPass();
  blit->AddCopy(texture, buffer);
  blit->EncodeCommands();
  auto latch = std::make_shared<fml::CountDownLatch>(1u);
  context->GetCommandQueue()->Submit(
      {command}, [latch](CommandBuffer::Status status) { latch->CountDown(); });
  latch->Wait();
  buffer->Invalidate();
  auto bytes = buffer->OnGetContents();
  return std::vector<uint8_t>(bytes, bytes + desc.size);
}

double ExternalAlpha(double alpha) {
  const double transparency = 1.0 - alpha;
  const double linear = transparency <= 0.04045
                            ? transparency / 12.92
                            : std::pow((transparency + 0.055) / 1.055, 2.4);
  return 1.0 - linear;
}

}  // namespace

TEST(ExternalCoverageTest, LayerCoverageCannotDistributeOpacity) {
  Paint paint;
  paint.color = Color::White().WithAlpha(0.58f);
  EXPECT_TRUE(Paint::CanApplyOpacityPeephole(paint));
  paint.coverage_mode = DlCoverageMode::kExternalLinearBackdrop;
  EXPECT_FALSE(Paint::CanApplyOpacityPeephole(paint));
}

// Compare against the actual unconverted GPU mask, not a software-rendered
// golden. Partial MSAA edge coverage must be included in the transfer and
// snapshot bounds must not change the device-pixel phase.
TEST_P(AiksTest, ExternalCoverageIncludesResolvedPathEdges) {
  for (float scale : {1.0f, 1.25f, 2.0f}) {
    for (bool stroke : {false, true}) {
      SCOPED_TRACE(scale);
      SCOPED_TRACE(stroke);
      DisplayListBuilder builder;
      auto path = DlPathBuilder()
                      .MoveTo({2.3f, 2.7f})
                      .LineTo({13.2f, 6.4f})
                      .LineTo({4.1f, 14.6f})
                      .Close()
                      .TakePath();
      DlPaint paint;
      paint.setColor(DlColor::ARGB(0.58f, 0.2f, 0.4f, 0.6f));
      if (stroke) {
        paint.setDrawStyle(DlDrawStyle::kStroke);
        paint.setStrokeWidth(1.6f);
      }
      for (int column = 0; column < 2; column++) {
        builder.Save();
        builder.Translate(column * 48.0f + 0.3f, 0.2f);
        builder.Scale(scale, scale);
        paint.setCoverageMode(column == 0
                                  ? DlCoverageMode::kPlatformDefault
                                  : DlCoverageMode::kExternalLinearBackdrop);
        builder.DrawPath(path, paint);
        builder.Restore();
      }
      AiksContext renderer(GetContext(), nullptr);
      auto texture =
          DisplayListToTexture(builder.Build(), ISize(96, 48), renderer);
      ASSERT_TRUE(texture);
      auto bytes = ReadPixels(GetContext(), texture);
      ASSERT_EQ(bytes.size(), 96u * 48u * 4u);
      int partial_edges = 0;
      for (int y = 0; y < 48; y++) {
        for (int x = 0; x < 48; x++) {
          auto a = bytes[(y * 96 + x) * 4 + 3];
          auto b = bytes[(y * 96 + x + 48) * 4 + 3];
          EXPECT_NEAR(b, 255.0 * ExternalAlpha(a / 255.0), 2.0)
              << "pixel " << x << "," << y;
          partial_edges += a > 0 && a < 140;
        }
      }
      EXPECT_GT(partial_edges, 5);
    }
  }
}

TEST_P(AiksTest, ExternalCoverageLayerAppliesOpacityOnceToOverlappingShapes) {
  DisplayListBuilder builder;
  DlPaint layer;
  layer.setColor(DlColor::kWhite().modulateOpacity(0.58f));
  layer.setCoverageMode(DlCoverageMode::kExternalLinearBackdrop);
  builder.SaveLayer(std::nullopt, &layer);
  DlPaint ink;
  ink.setColor(DlColor::kWhite());
  builder.DrawRect(DlRect::MakeLTRB(4, 4, 20, 12), ink);
  builder.DrawRect(DlRect::MakeLTRB(8, 8, 16, 24), ink);
  builder.Restore();
  AiksContext renderer(GetContext(), nullptr);
  auto texture = DisplayListToTexture(builder.Build(), ISize(32, 32), renderer);
  ASSERT_TRUE(texture);
  auto bytes = ReadPixels(GetContext(), texture);
  ASSERT_EQ(bytes.size(), 32u * 32u * 4u);
  const double expected = 255.0 * ExternalAlpha(0.58);
  for (const auto& point : {DlPoint(6, 6), DlPoint(10, 10), DlPoint(10, 20)}) {
    EXPECT_NEAR(bytes[(int(point.y) * 32 + int(point.x)) * 4 + 3], expected,
                2.0);
  }
}

TEST_P(AiksTest, ExternalCoverageLayerPreservesAnalyticEdges) {
  for (const auto color : {DlColor::kBlack(), DlColor::kWhite()}) {
    for (float scale : {1.0f, 1.25f, 2.0f}) {
      DisplayListBuilder builder;
      for (int column = 0; column < 2; column++) {
        builder.Save();
        builder.Translate(column * 48.0f + 0.3f, 0.2f);
        builder.Scale(scale, scale);
        DlPaint ink;
        ink.setColor(color);
        ink.setCoverageMode(DlCoverageMode::kExternalLinearBackdrop);
        if (column == 1) {
          builder.SaveLayer(std::nullopt, &ink);
          // A nested ordinary save must inherit the raw-mask scope.
          builder.Save();
          ink.setCoverageMode(DlCoverageMode::kPlatformDefault);
        }
        builder.DrawCircle(DlPoint(9.2f, 9.7f), 6.3f, ink);
        if (column == 1) {
          builder.Restore();
          builder.Restore();
        }
        builder.Restore();
      }
      AiksContext renderer(GetContext(), nullptr);
      auto texture =
          DisplayListToTexture(builder.Build(), ISize(96, 48), renderer);
      ASSERT_TRUE(texture);
      auto bytes = ReadPixels(GetContext(), texture);
      ASSERT_EQ(bytes.size(), 96u * 48u * 4u);
      int edges = 0;
      for (int y = 0; y < 48; y++) {
        for (int x = 0; x < 48; x++) {
          auto a = bytes[(y * 96 + x) * 4 + 3];
          auto b = bytes[(y * 96 + x + 48) * 4 + 3];
          EXPECT_NEAR(a, b, 2)
              << "pixel " << x << "," << y << " scale " << scale;
          edges += a > 0 && a < 250;
        }
      }
      EXPECT_GT(edges, 5);
    }
  }
}

TEST_P(AiksTest, ExternalCoveragePreservesDestructiveBlendGeometry) {
  for (auto blend : {DlBlendMode::kSrc, DlBlendMode::kClear}) {
    DisplayListBuilder builder;
    builder.DrawColor(DlColor::kBlue(), DlBlendMode::kSrc);
    auto path = DlPathBuilder()
                    .MoveTo({4, 4})
                    .LineTo({24, 4})
                    .LineTo({4, 24})
                    .Close()
                    .TakePath();
    for (int column = 0; column < 2; column++) {
      builder.Save();
      builder.Translate(column * 32.0f, 0);
      DlPaint ink;
      ink.setColor(DlColor::kRed().modulateOpacity(0.58f));
      ink.setBlendMode(blend);
      ink.setCoverageMode(column == 0
                              ? DlCoverageMode::kPlatformDefault
                              : DlCoverageMode::kExternalLinearBackdrop);
      builder.DrawPath(path, ink);
      builder.Restore();
    }
    AiksContext renderer(GetContext(), nullptr);
    auto texture =
        DisplayListToTexture(builder.Build(), ISize(64, 32), renderer);
    ASSERT_TRUE(texture);
    auto bytes = ReadPixels(GetContext(), texture);
    ASSERT_EQ(bytes.size(), 64u * 32u * 4u);
    for (int y = 0; y < 32; y++) {
      for (int x = 0; x < 32; x++) {
        for (int channel = 0; channel < 4; channel++) {
          EXPECT_EQ(bytes[(y * 64 + x) * 4 + channel],
                    bytes[(y * 64 + x + 32) * 4 + channel]);
        }
      }
    }
  }
}

TEST_P(AiksTest, ExternalCoverageFollowsFiltersAndNestedLayerOpacity) {
  const float half_alpha[20] = {1, 0, 0, 0, 0, 0, 1, 0, 0,    0,
                                0, 0, 1, 0, 0, 0, 0, 0, 0.5f, 0};
  DisplayListBuilder builder;
  for (int column = 0; column < 3; column++) {
    builder.Save();
    builder.Translate(column * 32.0f, 0);
    DlPaint ink;
    ink.setColor(DlColor::kBlack().modulateOpacity(0.58f));
    ink.setCoverageMode(DlCoverageMode::kExternalLinearBackdrop);
    ink.setColorFilter(DlColorFilter::MakeMatrix(half_alpha));
    if (column == 0) {
      auto path = DlPathBuilder()
                      .MoveTo({4, 4})
                      .LineTo({28, 4})
                      .LineTo({4, 28})
                      .Close()
                      .TakePath();
      builder.DrawPath(path, ink);
    } else if (column == 1) {
      builder.DrawCircle(DlPoint(12, 12), 10, ink);
    } else {
      builder.SaveLayer(std::nullopt, &ink);
      DlPaint inner;
      inner.setColor(DlColor::kBlack().modulateOpacity(0.5f));
      // A nested external request is still owned by the outer mask.
      inner.setCoverageMode(DlCoverageMode::kExternalLinearBackdrop);
      builder.SaveLayer(std::nullopt, &inner);
      inner.setColor(DlColor::kBlack());
      builder.DrawCircle(DlPoint(12, 12), 10, inner);
      builder.Restore();
      builder.Restore();
    }
    builder.Restore();
  }
  AiksContext renderer(GetContext(), nullptr);
  auto texture = DisplayListToTexture(builder.Build(), ISize(96, 32), renderer);
  ASSERT_TRUE(texture);
  auto bytes = ReadPixels(GetContext(), texture);
  ASSERT_EQ(bytes.size(), 96u * 32u * 4u);
  for (int column = 0; column < 3; column++) {
    const double alpha = 0.58 * 0.5 * (column == 2 ? 0.5 : 1);
    EXPECT_NEAR(bytes[(8 * 96 + column * 32 + 8) * 4 + 3],
                255.0 * ExternalAlpha(alpha), 2.0);
  }
}

}  // namespace impeller::testing
