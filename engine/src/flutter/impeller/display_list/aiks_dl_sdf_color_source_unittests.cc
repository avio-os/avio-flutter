// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Color sources on UberSDF shapes (Avio patch 52). These tests need SDF
// rendering: they run on the SDF playground backends and on Vulkan with SDFs
// enabled (the only multisampled one on Linux), and skip elsewhere.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "display_list/dl_builder.h"
#include "display_list/dl_color.h"
#include "display_list/dl_sampling_options.h"
#include "display_list/dl_tile_mode.h"
#include "display_list/effects/dl_color_source.h"
#include "display_list/effects/dl_image_filter.h"
#include "fml/synchronization/count_down_latch.h"
#include "impeller/core/device_buffer.h"
#include "impeller/display_list/aiks_context.h"
#include "impeller/display_list/aiks_unittests.h"
#include "impeller/display_list/dl_dispatcher.h"
#include "impeller/display_list/paint.h"
#include "impeller/entity/contents/filters/color_filter_contents.h"
#include "impeller/entity/contents/filters/inputs/filter_input.h"
#include "impeller/entity/contents/uber_sdf_contents.h"
#include "impeller/entity/contents/uber_sdf_parameters.h"
#include "impeller/entity/entity.h"
#include "impeller/entity/geometry/uber_sdf_geometry.h"
#include "impeller/entity/render_target_cache.h"
#include "impeller/geometry/rounding_radii.h"
#include "impeller/renderer/snapshot.h"

namespace impeller::testing {
namespace {

using namespace flutter;

// Records every offscreen request, then defers to the real cache.
class CountingRenderTargetCache final : public RenderTargetCache {
 public:
  struct Request {
    bool msaa = false;
    bool depth_stencil = false;
  };

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
    requests.push_back(
        {.msaa = false,
         .depth_stencil = stencil_attachment_config.has_value()});
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
    requests.push_back(
        {.msaa = true, .depth_stencil = stencil_attachment_config.has_value()});
    return RenderTargetCache::CreateOffscreenMSAA(
        context, size, mip_count, label, color_attachment_config,
        stencil_attachment_config, existing_color_msaa_texture,
        existing_color_resolve_texture, existing_depth_stencil_texture,
        target_pixel_format);
  }

  std::vector<Request> requests;
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

// A color source UberSDF never shades itself, so it always takes the masked
// fallback this patch slims.
std::shared_ptr<DlColorSource> MakeSweep() {
  static const DlColor kColors[] = {DlColor::kRed(), DlColor::kGreen(),
                                    DlColor::kBlue(), DlColor::kRed()};
  static const float kStops[] = {0.0f, 0.33f, 0.66f, 1.0f};
  return DlColorSource::MakeSweep({50, 30}, 0, 360, 4, kColors, kStops,
                                  DlTileMode::kClamp);
}

constexpr ISize kTargetSize(100, 60);

// The bytes of the rows [top, bottom) and columns [left, right) of an
// RGBA8 readback of a kTargetSize texture.
std::vector<uint8_t> PixelRegion(const std::vector<uint8_t>& bytes,
                                 int left,
                                 int top,
                                 int right,
                                 int bottom) {
  std::vector<uint8_t> region;
  for (int y = top; y < bottom; y++) {
    for (int x = left; x < right; x++) {
      const size_t offset = (y * kTargetSize.width + x) * 4;
      region.insert(region.end(), bytes.begin() + offset,
                    bytes.begin() + offset + 4);
    }
  }
  return region;
}

int MaxDifference(const std::vector<uint8_t>& a,
                  const std::vector<uint8_t>& b) {
  int max_difference = a.size() == b.size() ? 0 : 255;
  for (size_t i = 0; i < std::min(a.size(), b.size()); i++) {
    max_difference = std::max(max_difference, std::abs(int(a[i]) - int(b[i])));
  }
  return max_difference;
}

}  // namespace

// Red before EN52: the shader rect was masked by the SDF through two
// multisampled snapshots and a blend target.
TEST_P(AiksTest, ShaderRectContainingClipAllocatesNoOffscreen) {
  if (!EnsureContextUsesSDFs()) {
    GTEST_SKIP() << "Requires SDF rendering.";
  }
  auto cache = std::make_shared<CountingRenderTargetCache>(
      GetContext()->GetResourceAllocator());
  AiksContext renderer(GetContext(), nullptr, cache);

  DisplayListBuilder builder;
  builder.ClipRect(DlRect::MakeLTRB(10, 10, 90, 50));
  DlPaint paint;
  paint.setColorSource(MakeSweep());
  builder.DrawRect(DlRect::MakeLTRB(-20, -20, 120, 80), paint);
  auto texture = DisplayListToTexture(builder.Build(), kTargetSize, renderer);
  ASSERT_TRUE(texture);
  EXPECT_TRUE(cache->requests.empty());
}

// Red before EN52: both inputs were snapshotted multisampled with
// depth/stencil.
TEST_P(AiksTest, ShaderRRectInsideClipStillMasks) {
  if (!EnsureContextUsesSDFs()) {
    GTEST_SKIP() << "Requires SDF rendering.";
  }
  auto cache = std::make_shared<CountingRenderTargetCache>(
      GetContext()->GetResourceAllocator());
  AiksContext renderer(GetContext(), nullptr, cache);

  DisplayListBuilder builder;
  DlPaint paint;
  paint.setColorSource(MakeSweep());
  builder.DrawRoundRect(
      DlRoundRect::MakeRectXY(DlRect::MakeLTRB(10, 10, 90, 50), 8, 8), paint);
  auto texture = DisplayListToTexture(builder.Build(), kTargetSize, renderer);
  ASSERT_TRUE(texture);

  // Two input snapshots and the blend target, all single-sample without
  // depth/stencil.
  EXPECT_EQ(cache->requests.size(), 3u);
  for (const auto& request : cache->requests) {
    EXPECT_FALSE(request.msaa);
    EXPECT_FALSE(request.depth_stencil);
  }

  // The mask still shapes the fill: corners are empty, edges antialiased.
  auto bytes = ReadPixels(GetContext(), texture);
  ASSERT_EQ(bytes.size(), static_cast<size_t>(kTargetSize.Area() * 4));
  const auto alpha = [&](int x, int y) {
    return bytes[(y * kTargetSize.width + x) * 4 + 3];
  };
  EXPECT_EQ(alpha(10, 10), 0u);
  EXPECT_EQ(alpha(50, 30), 255u);
  int partial = 0;
  for (int x = 0; x < kTargetSize.width; x++) {
    partial += alpha(x, 10) > 0 && alpha(x, 10) < 255;
  }
  EXPECT_GT(partial, 0);
}

// A shader rect that contains the clip draws exactly what drawPaint draws
// under the same clip.
TEST_P(AiksTest, ShaderRectContainingClipMatchesDrawPaint) {
  if (!EnsureContextUsesSDFs()) {
    GTEST_SKIP() << "Requires SDF rendering.";
  }
  AiksContext renderer(GetContext(), nullptr);
  DlPaint paint;
  paint.setColorSource(MakeSweep());
  const auto render = [&](bool use_rect) {
    DisplayListBuilder builder;
    builder.ClipRect(DlRect::MakeLTRB(10.5, 10, 90, 50.25));
    if (use_rect) {
      builder.DrawRect(DlRect::MakeLTRB(-20, -20, 120, 80), paint);
    } else {
      builder.DrawPaint(paint);
    }
    auto texture = DisplayListToTexture(builder.Build(), kTargetSize, renderer);
    return texture ? ReadPixels(GetContext(), texture) : std::vector<uint8_t>{};
  };
  const auto rect_pixels = render(true);
  const auto paint_pixels = render(false);
  ASSERT_EQ(rect_pixels.size(), static_cast<size_t>(kTargetSize.Area() * 4));
  ASSERT_EQ(rect_pixels.size(), paint_pixels.size());
  for (size_t i = 0; i < rect_pixels.size(); i++) {
    ASSERT_LE(std::abs(int(rect_pixels[i]) - int(paint_pixels[i])), 1)
        << "byte " << i;
  }
}

// (a) draws what the masked composite it replaces drew. The rect's left edge
// (5.5) is inside the target. A clip inside the rect takes (a); a clip that
// reaches past that edge keeps the mask. Where both clips let pixels through,
// every pixel center lies at least 4.5 pixels inside the edge, so the two
// renders must agree.
TEST_P(AiksTest, ShaderRectContainingClipMatchesMaskedComposite) {
  if (!EnsureContextUsesSDFs()) {
    GTEST_SKIP() << "Requires SDF rendering.";
  }
  AiksContext renderer(GetContext(), nullptr);
  DlPaint paint;
  paint.setColorSource(MakeSweep());
  const auto render = [&](const DlRect& clip) {
    DisplayListBuilder builder;
    builder.ClipRect(clip);
    builder.DrawRect(DlRect::MakeLTRB(5.5, -20, 120, 80), paint);
    auto texture = DisplayListToTexture(builder.Build(), kTargetSize, renderer);
    return texture ? ReadPixels(GetContext(), texture) : std::vector<uint8_t>{};
  };
  const auto unmasked = render(DlRect::MakeLTRB(10, 10, 90, 50));
  const auto masked = render(DlRect::MakeLTRB(0, 10, 90, 50));
  ASSERT_EQ(unmasked.size(), static_cast<size_t>(kTargetSize.Area() * 4));
  ASSERT_EQ(masked.size(), unmasked.size());
  EXPECT_LE(MaxDifference(PixelRegion(unmasked, 10, 10, 90, 50),
                          PixelRegion(masked, 10, 10, 90, 50)),
            1);
}

// Red before the fix: (a) skipped the mask for a rect containing the clip
// even when the paint had an image filter. The filter moves the rect's left
// edge from 5.5 to 25.5, inside the clip, after the containment test, and
// that edge was then drawn as rect geometry instead of the SDF's analytic
// edge (aliased single-sample, or plain 4x coverage under MSAA).
TEST_P(AiksTest, ShaderRectWithImageFilterKeepsMask) {
  if (!EnsureContextUsesSDFs()) {
    GTEST_SKIP() << "Requires SDF rendering.";
  }
  AiksContext renderer(GetContext(), nullptr);
  DlPaint paint;
  paint.setColorSource(MakeSweep());
  paint.setImageFilter(
      DlImageFilter::MakeMatrix(DlMatrix::MakeTranslation({20.0f, 0.0f, 0.0f}),
                                DlImageSampling::kNearestNeighbor));
  const auto render = [&](const DlRect& clip) {
    DisplayListBuilder builder;
    builder.ClipRect(clip);
    builder.DrawRect(DlRect::MakeLTRB(5.5, -20, 120, 80), paint);
    auto texture = DisplayListToTexture(builder.Build(), kTargetSize, renderer);
    return texture ? ReadPixels(GetContext(), texture) : std::vector<uint8_t>{};
  };
  // The rect contains this clip before the filter moves it.
  const auto contained = render(DlRect::MakeLTRB(10, 10, 90, 50));
  // The rect does not contain this one, so it is masked either way.
  const auto masked = render(DlRect::MakeLTRB(0, 10, 90, 50));
  ASSERT_EQ(contained.size(), static_cast<size_t>(kTargetSize.Area() * 4));
  ASSERT_EQ(masked.size(), contained.size());
  EXPECT_LE(MaxDifference(PixelRegion(contained, 10, 10, 90, 50),
                          PixelRegion(masked, 10, 10, 90, 50)),
            2);
  // The moved edge is inside the compared region and analytically
  // antialiased: the opaque sweep has partially covered pixels on a row.
  int partial = 0;
  for (int x = 10; x < 90; x++) {
    const uint8_t alpha = contained[(30 * kTargetSize.width + x) * 4 + 3];
    partial += alpha > 0 && alpha < 255;
  }
  EXPECT_GT(partial, 0);
}

// (b) against the composite it replaces: the same kSrcIn blend of a white
// UberSDF rounded-rect mask and a sweep gradient, built as Canvas built it
// before patch 52 (both inputs snapshotted multisampled with depth/stencil)
// and as it builds it now (single-sample, no depth/stencil). The multisampled
// inputs are only meaningful where offscreen MSAA exists (Vulkan here); on
// single-sample backends both renders differ only in depth/stencil.
TEST_P(AiksTest, SingleSampleMaskInputsMatchMultisampledInputs) {
  if (!EnsureContextUsesSDFs()) {
    GTEST_SKIP() << "Requires SDF rendering.";
  }
  AiksContext aiks_context(GetContext(), nullptr);
  const ContentContext& renderer = aiks_context.GetContentContext();
  const std::shared_ptr<DlColorSource> sweep = MakeSweep();
  const auto composite = [&](bool multisampled_inputs) {
    const UberSDFParameters params = UberSDFParameters::MakeRoundedRect(
        Color::White(), Rect::MakeLTRB(10, 10, 90, 50),
        RoundingRadii::MakeRadius(8), std::nullopt);
    std::shared_ptr<UberSDFContents> mask = UberSDFContents::Make(
        params, std::make_unique<UberSDFGeometry>(params));
    mask->SetCoverageMode(flutter::DlCoverageMode::kPlatformDefault);
    mask->SetDeferCoverageTransform(true);
    impeller::Paint source_paint;
    source_paint.color_source = sweep.get();
    std::shared_ptr<ColorSourceContents> source =
        source_paint.CreateContents(renderer, mask->GetGeometry());
    std::shared_ptr<Contents> blend = ColorFilterContents::MakeBlend(
        BlendMode::kSrcIn,
        {FilterInput::Make(mask, multisampled_inputs, multisampled_inputs),
         FilterInput::Make(source, multisampled_inputs, multisampled_inputs)});
    return blend->RenderToSnapshot(renderer, Entity{}, {});
  };
  const std::optional<Snapshot> before =
      composite(/*multisampled_inputs=*/true);
  const std::optional<Snapshot> after =
      composite(/*multisampled_inputs=*/false);
  ASSERT_TRUE(before.has_value() && before->texture);
  ASSERT_TRUE(after.has_value() && after->texture);
  ASSERT_EQ(before->texture->GetSize(), after->texture->GetSize());
  EXPECT_EQ(before->transform, after->transform);
  const auto before_pixels = ReadPixels(GetContext(), before->texture);
  const auto after_pixels = ReadPixels(GetContext(), after->texture);
  ASSERT_FALSE(before_pixels.empty());
  EXPECT_LE(MaxDifference(before_pixels, after_pixels), 1);
  // Not vacuous: the composite has an empty corner and antialiased edges.
  int partial = 0;
  for (size_t i = 3; i < after_pixels.size(); i += 4) {
    partial += after_pixels[i] > 0 && after_pixels[i] < 255;
  }
  EXPECT_GT(partial, 0);
  EXPECT_EQ(after_pixels[3], 0u);
}

}  // namespace impeller::testing
