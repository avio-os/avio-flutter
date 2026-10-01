// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Linear and radial gradients shaded inside UberSDF (Avio patch 50, a
// backport of flutter#192124 and #192962). These tests need SDF rendering:
// they run on the SDF playground backends (the ramp-texture variant on
// OpenGL ES) and on Vulkan with SDFs enabled (the storage-buffer variant
// Avio ships), and skip elsewhere.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "display_list/dl_builder.h"
#include "display_list/dl_color.h"
#include "display_list/dl_paint.h"
#include "display_list/dl_tile_mode.h"
#include "display_list/effects/dl_color_source.h"
#include "fml/synchronization/count_down_latch.h"
#include "impeller/core/device_buffer.h"
#include "impeller/display_list/aiks_context.h"
#include "impeller/display_list/aiks_unittests.h"
#include "impeller/display_list/dl_dispatcher.h"
#include "impeller/entity/render_target_cache.h"

namespace impeller::testing {
namespace {

using namespace flutter;

// Counts every offscreen the canvas requests, then defers to the real cache.
class OffscreenCountingCache final : public RenderTargetCache {
 public:
  using RenderTargetCache::RenderTargetCache;

  RenderTarget CreateOffscreen(
      const Context& context,
      ISize size,
      int mip_count,
      std::string_view label = "Offscreen",
      RenderTarget::AttachmentConfig color_attachment_config =
          RenderTarget::kDefaultColorAttachmentConfig,
      std::optional<RenderTarget::AttachmentConfig> stencil_attachment_config =
          RenderTarget::kDefaultStencilAttachmentConfig,
      const std::shared_ptr<Texture>& existing_color_texture = nullptr,
      const std::shared_ptr<Texture>& existing_depth_stencil_texture = nullptr,
      std::optional<PixelFormat> target_pixel_format = std::nullopt) override {
    requests++;
    return RenderTargetCache::CreateOffscreen(
        context, size, mip_count, label, color_attachment_config,
        stencil_attachment_config, existing_color_texture,
        existing_depth_stencil_texture, target_pixel_format);
  }

  RenderTarget CreateOffscreenMSAA(
      const Context& context,
      ISize size,
      int mip_count,
      std::string_view label = "Offscreen MSAA",
      RenderTarget::AttachmentConfigMSAA color_attachment_config =
          RenderTarget::kDefaultColorAttachmentConfigMSAA,
      std::optional<RenderTarget::AttachmentConfig> stencil_attachment_config =
          RenderTarget::kDefaultStencilAttachmentConfig,
      const std::shared_ptr<Texture>& existing_color_msaa_texture = nullptr,
      const std::shared_ptr<Texture>& existing_color_resolve_texture = nullptr,
      const std::shared_ptr<Texture>& existing_depth_stencil_texture = nullptr,
      std::optional<PixelFormat> target_pixel_format = std::nullopt) override {
    requests++;
    return RenderTargetCache::CreateOffscreenMSAA(
        context, size, mip_count, label, color_attachment_config,
        stencil_attachment_config, existing_color_msaa_texture,
        existing_color_resolve_texture, existing_depth_stencil_texture,
        target_pixel_format);
  }

  size_t requests = 0u;
};

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

constexpr ISize kTargetSize(128, 96);

// A horizontal gradient. Its color depends only on x, so a local matrix that
// scales y leaves its field unchanged; patch 50 shades only similarity
// matrices in UberSDF, so that matrix routes the same gradient through the
// masked kSrcIn path every color source used before patch 50.
std::shared_ptr<DlColorSource> MakeHorizontalGradient(bool masked,
                                                      bool translucent) {
  static const DlColor kOpaque[] = {DlColor::ARGB(1.0, 0.08, 0.09, 0.12),
                                    DlColor::ARGB(1.0, 0.85, 0.88, 0.95)};
  // A glass sheen: translucent white fading out.
  static const DlColor kGlass[] = {DlColor::ARGB(0.35, 1.0, 1.0, 1.0),
                                   DlColor::ARGB(0.05, 1.0, 1.0, 1.0)};
  static const float kStops[] = {0.0f, 1.0f};
  const DlMatrix scale_y = DlMatrix::MakeScale({1.0f, 2.0f, 1.0f});
  return DlColorSource::MakeLinear(
      {8.0f, 0.0f}, {120.0f, 0.0f}, 2, translucent ? kGlass : kOpaque, kStops,
      DlTileMode::kClamp, masked ? &scale_y : nullptr);
}

enum class Shape { kRect, kRoundRect, kOval, kCircle };

void DrawShape(DisplayListBuilder& builder, Shape shape, const DlPaint& paint) {
  const DlRect bounds = DlRect::MakeLTRB(16, 16, 112, 80);
  switch (shape) {
    case Shape::kRect:
      builder.DrawRect(bounds, paint);
      return;
    case Shape::kRoundRect:
      builder.DrawRoundRect(DlRoundRect::MakeRectXY(bounds, 14, 14), paint);
      return;
    case Shape::kOval:
      builder.DrawOval(bounds, paint);
      return;
    case Shape::kCircle:
      builder.DrawCircle({64, 48}, 30, paint);
      return;
  }
}

}  // namespace

// Red before patch 50: the gradient rect was blended through two snapshots
// and a blend target.
TEST_P(AiksTest, SdfLinearGradientRectAllocatesNoOffscreen) {
  if (!EnsureContextUsesSDFs()) {
    GTEST_SKIP() << "Requires SDF rendering.";
  }
  auto cache = std::make_shared<OffscreenCountingCache>(
      GetContext()->GetResourceAllocator());
  AiksContext renderer(GetContext(), nullptr, cache);

  DisplayListBuilder builder;
  DlPaint paint;
  paint.setColorSource(MakeHorizontalGradient(/*masked=*/false, false));
  DrawShape(builder, Shape::kRect, paint);
  ASSERT_TRUE(DisplayListToTexture(builder.Build(), kTargetSize, renderer));
  EXPECT_EQ(cache->requests, 0u);
}

// The dock light pool: a radial gradient circle under a non-uniform canvas
// scale. UberSDF shades in local space, so the canvas transform is free.
TEST_P(AiksTest,
       SdfRadialGradientCircleUnderNonUniformScaleAllocatesNoOffscreen) {
  if (!EnsureContextUsesSDFs()) {
    GTEST_SKIP() << "Requires SDF rendering.";
  }
  auto cache = std::make_shared<OffscreenCountingCache>(
      GetContext()->GetResourceAllocator());
  AiksContext renderer(GetContext(), nullptr, cache);

  static const DlColor kColors[] = {DlColor::ARGB(0.6, 1.0, 1.0, 1.0),
                                    DlColor::ARGB(0.0, 1.0, 1.0, 1.0)};
  static const float kStops[] = {0.0f, 1.0f};
  DlPaint paint;
  paint.setColorSource(DlColorSource::MakeRadial({32, 24}, 20, 2, kColors,
                                                 kStops, DlTileMode::kClamp));
  DisplayListBuilder builder;
  builder.Scale(2.0f, 1.0f);
  builder.DrawCircle({32, 24}, 20, paint);
  ASSERT_TRUE(DisplayListToTexture(builder.Build(), kTargetSize, renderer));
  EXPECT_EQ(cache->requests, 0u);
}

TEST_P(AiksTest, UnsupportedColorSourceStillBlends) {
  if (!EnsureContextUsesSDFs()) {
    GTEST_SKIP() << "Requires SDF rendering.";
  }
  auto cache = std::make_shared<OffscreenCountingCache>(
      GetContext()->GetResourceAllocator());
  AiksContext renderer(GetContext(), nullptr, cache);

  static const DlColor kColors[] = {DlColor::kRed(), DlColor::kBlue()};
  static const float kStops[] = {0.0f, 1.0f};
  DlPaint paint;
  paint.setColorSource(DlColorSource::MakeSweep({64, 48}, 0, 360, 2, kColors,
                                                kStops, DlTileMode::kClamp));
  DisplayListBuilder builder;
  DrawShape(builder, Shape::kRoundRect, paint);
  ASSERT_TRUE(DisplayListToTexture(builder.Build(), kTargetSize, renderer));
  // Two input snapshots and the blend target.
  EXPECT_EQ(cache->requests, 3u);
}

// Avio's look must not change. Every pixel of a gradient shape shaded inside
// UberSDF, including rounded corners, analytic edges and translucent glass
// edges in both coverage modes, matches the masked composite every gradient
// shape used before patch 50:
// - alpha (the edge coverage) within 2/255: one extra 8-bit quantization in
//   the old two-texture composite;
// - color within 2/255 on the ramp-texture variant, which does not dither,
//   and within 5/255 on the storage-buffer variant, which dithers like the
//   storage-buffer gradient shaders the composite used: the composite
//   dithered in its snapshot's pixel grid, up to +/-1.96/255 each way,
//   before that extra quantization;
// - every pixel the composite leaves fully transparent stays exactly
//   transparent, so nothing (dither included) leaks outside the shape, such
//   as into the corners of the quad around a rounded shape.
TEST_P(AiksTest, SdfGradientEdgesMatchMaskedComposite) {
  if (!EnsureContextUsesSDFs()) {
    GTEST_SKIP() << "Requires SDF rendering.";
  }
  AiksContext renderer(GetContext(), nullptr);
  const bool dithered = GetContext()->GetCapabilities()->SupportsSSBO();
  const int max_color_difference = dithered ? 5 : 2;
  const auto render = [&](Shape shape, bool masked, bool translucent,
                          DlCoverageMode mode) {
    DisplayListBuilder builder;
    DlPaint paint;
    paint.setColorSource(MakeHorizontalGradient(masked, translucent));
    paint.setCoverageMode(mode);
    DrawShape(builder, shape, paint);
    auto texture = DisplayListToTexture(builder.Build(), kTargetSize, renderer);
    return texture ? ReadPixels(GetContext(), texture) : std::vector<uint8_t>{};
  };

  for (Shape shape :
       {Shape::kRect, Shape::kRoundRect, Shape::kOval, Shape::kCircle}) {
    for (bool translucent : {false, true}) {
      for (DlCoverageMode mode : {DlCoverageMode::kPlatformDefault,
                                  DlCoverageMode::kExternalLinearBackdrop}) {
        SCOPED_TRACE(static_cast<int>(shape));
        SCOPED_TRACE(translucent);
        SCOPED_TRACE(static_cast<int>(mode));
        const auto shaded = render(shape, /*masked=*/false, translucent, mode);
        const auto reference =
            render(shape, /*masked=*/true, translucent, mode);
        ASSERT_EQ(shaded.size(), static_cast<size_t>(kTargetSize.Area() * 4));
        ASSERT_EQ(shaded.size(), reference.size());

        int max_alpha_difference = 0;
        int max_rgb_difference = 0;
        int leaked_pixels = 0;
        int partial_pixels = 0;
        for (size_t pixel = 0; pixel < shaded.size(); pixel += 4) {
          bool reference_empty = true;
          bool shaded_empty = true;
          for (size_t channel = 0; channel < 4; channel++) {
            const int difference = std::abs(int(shaded[pixel + channel]) -
                                            int(reference[pixel + channel]));
            if (channel == 3) {
              max_alpha_difference = std::max(max_alpha_difference, difference);
            } else {
              max_rgb_difference = std::max(max_rgb_difference, difference);
            }
            reference_empty &= reference[pixel + channel] == 0;
            shaded_empty &= shaded[pixel + channel] == 0;
          }
          leaked_pixels += reference_empty && !shaded_empty;
          partial_pixels += shaded[pixel + 3] > 0 && shaded[pixel + 3] < 255;
        }
        EXPECT_LE(max_alpha_difference, 2);
        EXPECT_LE(max_rgb_difference, max_color_difference);
        EXPECT_EQ(leaked_pixels, 0);
        // The comparison covers antialiased edges, not only interiors.
        if (!translucent) {
          EXPECT_GT(partial_pixels, 20);
        }
      }
    }
  }
}

// Nothing is drawn where the shape has no coverage: the corners of the quad
// around the circle are exactly transparent in both coverage modes, on
// either variant, whatever the dither.
TEST_P(AiksTest, SdfGradientLeavesUncoveredQuadPixelsTransparent) {
  if (!EnsureContextUsesSDFs()) {
    GTEST_SKIP() << "Requires SDF rendering.";
  }
  AiksContext renderer(GetContext(), nullptr);
  for (DlCoverageMode mode : {DlCoverageMode::kPlatformDefault,
                              DlCoverageMode::kExternalLinearBackdrop}) {
    for (bool translucent : {false, true}) {
      SCOPED_TRACE(static_cast<int>(mode));
      SCOPED_TRACE(translucent);
      DisplayListBuilder builder;
      DlPaint paint;
      paint.setColorSource(
          MakeHorizontalGradient(/*masked=*/false, translucent));
      paint.setCoverageMode(mode);
      DrawShape(builder, Shape::kCircle, paint);
      auto texture =
          DisplayListToTexture(builder.Build(), kTargetSize, renderer);
      ASSERT_TRUE(texture);
      const auto bytes = ReadPixels(GetContext(), texture);
      ASSERT_EQ(bytes.size(), static_cast<size_t>(kTargetSize.Area() * 4));
      // The circle is centered at (64, 48) with radius 30. Each 3x3 corner
      // block of its bounding square lies more than 8 pixels outside it,
      // inside the quad UberSDF draws; the blocks cover several dither
      // phases in x and y.
      int nonzero = 0;
      for (int y : {18, 19, 20, 76, 77, 78}) {
        for (int x : {34, 35, 36, 92, 93, 94}) {
          for (int channel = 0; channel < 4; channel++) {
            nonzero += bytes[(y * kTargetSize.width + x) * 4 + channel] != 0;
          }
        }
      }
      EXPECT_EQ(nonzero, 0);
    }
  }
}

}  // namespace impeller::testing
