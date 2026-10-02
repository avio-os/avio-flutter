// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A backdrop filter inside another layer flips that layer's pass target
// (Avio patch 48). On backends that cannot read from a resolve texture
// (Vulkan) the flip swaps in a lazily allocated single-sample secondary.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "display_list/dl_builder.h"
#include "display_list/dl_color.h"
#include "display_list/dl_paint.h"
#include "display_list/dl_vertices.h"
#include "display_list/effects/dl_color_filter.h"
#include "display_list/effects/dl_color_sources.h"
#include "display_list/effects/dl_image_filter.h"
#include "fml/synchronization/count_down_latch.h"
#include "impeller/core/device_buffer.h"
#include "impeller/display_list/aiks_context.h"
#include "impeller/display_list/aiks_unittests.h"
#include "impeller/display_list/dl_dispatcher.h"
#include "impeller/display_list/dl_image_impeller.h"
#include "impeller/entity/contents/framebuffer_blend_contents.h"
#include "impeller/entity/contents/linear_gradient_contents.h"
#include "impeller/entity/geometry/rect_geometry.h"
#include "impeller/entity/render_target_cache.h"

namespace impeller::testing {

class CanvasBlendTestPeer {
 public:
  static void DrawScreen(Canvas& canvas,
                         std::shared_ptr<Contents> contents,
                         bool previous_fetch_path) {
    Entity entity;
    entity.SetTransform(canvas.GetCurrentTransform());
    if (previous_fetch_path) {
      auto fetch = std::make_shared<FramebufferBlendContents>();
      fetch->SetChildContents(std::move(contents));
      fetch->SetBlendMode(BlendMode::kScreen);
      entity.SetContents(std::move(fetch));
      entity.SetBlendMode(BlendMode::kSrc);
    } else {
      entity.SetContents(std::move(contents));
      entity.SetBlendMode(BlendMode::kScreen);
    }
    canvas.AddRenderEntityToCurrentPass(entity);
  }
};

namespace {

std::vector<uint8_t> ReadBackdropPixels(
    const std::shared_ptr<Context>& context,
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
  if (!command) {
    return {};
  }
  auto blit = command->CreateBlitPass();
  if (!blit || !blit->AddCopy(texture, buffer) || !blit->EncodeCommands()) {
    return {};
  }
  auto latch = std::make_shared<fml::CountDownLatch>(1u);
  auto finished = std::make_shared<std::atomic<bool>>(false);
  auto succeeded = std::make_shared<std::atomic<bool>>(false);
  if (!context->GetCommandQueue()
           ->Submit({command},
                    [latch, finished, succeeded](CommandBuffer::Status status) {
                      if (status != CommandBuffer::Status::kPending &&
                          !finished->exchange(true)) {
                        succeeded->store(status ==
                                         CommandBuffer::Status::kCompleted);
                        latch->CountDown();
                      }
                    })
           .ok()) {
    return {};
  }
  latch->Wait();
  if (!succeeded->load()) {
    return {};
  }
  buffer->Invalidate();
  auto bytes = buffer->OnGetContents();
  if (!bytes) {
    return {};
  }
  return std::vector<uint8_t>(bytes, bytes + desc.size);
}

// Records every offscreen request, including fetch source snapshots on Vulkan.
class LabelRecordingCache final : public RenderTargetCache {
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
    labels.emplace_back(label);
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
    labels.emplace_back(label);
    return RenderTargetCache::CreateOffscreenMSAA(
        context, size, mip_count, label, color_attachment_config,
        stencil_attachment_config, existing_color_msaa_texture,
        existing_color_resolve_texture, existing_depth_stencil_texture,
        target_pixel_format);
  }

  std::vector<std::string> labels;
};

}  // namespace

// The Shell's snap overlay shape: a backdrop filter inside an opacity layer.
// The backdrop filter swaps red and blue, so the layer's red square, read
// back through the flipped target, becomes blue; a green square drawn after
// the backdrop lands in the swapped-in secondary. The result must equal the
// same picture drawn directly, without a backdrop and without a flip. Before
// patch 48 the secondary was the resolve texture of a fresh MSAA target in
// the default color format; it is now one single-sample texture in the
// resolve texture's own format and storage.
TEST_P(AiksTest, BackdropInsideOpacityLayerMatchesDirectDraw) {
  using flutter::DisplayListBuilder;
  using flutter::DlColor;
  using flutter::DlColorFilter;
  using flutter::DlImageFilter;
  using flutter::DlPaint;
  using flutter::DlRect;

  constexpr ISize kSize(100, 60);
  auto cache = std::make_shared<LabelRecordingCache>(
      GetContext()->GetResourceAllocator());
  AiksContext renderer(GetContext(), nullptr, cache);
  DlPaint layer_paint;
  layer_paint.setOpacity(0.5f);
  DlPaint red;
  red.setColor(DlColor::kRed());
  DlPaint green;
  green.setColor(DlColor::kGreen());
  DlPaint blue;
  blue.setColor(DlColor::kBlue());
  // Swaps the red and blue channels.
  const float kSwapRedBlue[20] = {
      0, 0, 1, 0, 0,  //
      0, 1, 0, 0, 0,  //
      1, 0, 0, 0, 0,  //
      0, 0, 0, 1, 0,  //
  };
  const std::shared_ptr<DlImageFilter> backdrop =
      DlImageFilter::MakeColorFilter(DlColorFilter::MakeMatrix(kSwapRedBlue));
  ASSERT_TRUE(backdrop);

  DisplayListBuilder flipped;
  flipped.SaveLayer(std::nullopt, &layer_paint);
  flipped.DrawRect(DlRect::MakeLTRB(10, 10, 40, 40), red);
  flipped.SaveLayer(std::nullopt, nullptr, backdrop.get());
  flipped.DrawRect(DlRect::MakeLTRB(45, 15, 55, 25), green);
  flipped.Restore();
  flipped.Restore();

  DisplayListBuilder direct;
  direct.SaveLayer(std::nullopt, &layer_paint);
  direct.DrawRect(DlRect::MakeLTRB(10, 10, 40, 40), blue);
  direct.DrawRect(DlRect::MakeLTRB(45, 15, 55, 25), green);
  direct.Restore();

  auto flipped_texture = DisplayListToTexture(flipped.Build(), kSize, renderer);
  ASSERT_TRUE(flipped_texture);
  // Where the flip swaps resolve textures, it took the single-sample
  // secondary (red before patch 48: CreateOffscreenMSAA, no such label).
  const auto& caps = GetContext()->GetCapabilities();
  if (caps->SupportsOffscreenMSAA() && !caps->SupportsReadFromResolve() &&
      !caps->SupportsImplicitResolvingMSAA()) {
    EXPECT_NE(std::find(cache->labels.begin(), cache->labels.end(),
                        "EntityPassTarget Secondary"),
              cache->labels.end());
  }
  auto direct_texture = DisplayListToTexture(direct.Build(), kSize, renderer);
  ASSERT_TRUE(direct_texture);
  const auto flipped_pixels = ReadBackdropPixels(GetContext(), flipped_texture);
  const auto direct_pixels = ReadBackdropPixels(GetContext(), direct_texture);
  ASSERT_EQ(flipped_pixels.size(), static_cast<size_t>(kSize.Area() * 4));
  ASSERT_EQ(flipped_pixels.size(), direct_pixels.size());
  int max_difference = 0;
  for (size_t i = 0; i < flipped_pixels.size(); i++) {
    max_difference = std::max(max_difference, std::abs(int(flipped_pixels[i]) -
                                                       int(direct_pixels[i])));
  }
  EXPECT_LE(max_difference, 1);
  // Not vacuous: the squares are drawn at half opacity.
  const size_t center = (25 * kSize.width + 25) * 4;
  EXPECT_GT(flipped_pixels[center + 3], 0u);
  EXPECT_LT(flipped_pixels[center + 3], 255u);
}

// Rendered rectangles keep the pipeline-factor check independent of the
// image/vertex coefficient shaders. The root texture is allocated by
// DisplayListToTexture's separate allocator; any cache request is extra work.
TEST_P(AiksTest, ScreenPipelineMatchesCpuOracleWithoutOffscreen) {
  using namespace flutter;
  constexpr ISize kSize(100, 100);
  const std::array<DlColor, 5> colors = {
      DlColor(0x00000000), DlColor(0xFFFFFFFF), DlColor(0x2040BF80),
      DlColor(0xBFFF4000), DlColor(0x800080FF),
  };
  auto cache = std::make_shared<LabelRecordingCache>(
      GetContext()->GetResourceAllocator());
  AiksContext renderer(GetContext(), nullptr, cache);
  DisplayListBuilder blended;
  DisplayListBuilder reference;
  for (size_t y = 0; y < colors.size(); y++) {
    for (size_t x = 0; x < colors.size(); x++) {
      const auto rect = DlRect::MakeXYWH(x * 20, y * 20, 20, 20);
      blended.DrawRect(
          rect, DlPaint().setColor(colors[y]).setBlendMode(DlBlendMode::kSrc));
      blended.DrawRect(rect, DlPaint().setColor(colors[x]).setBlendMode(
                                 DlBlendMode::kScreen));
      const Color src(colors[x].getRedF(), colors[x].getGreenF(),
                      colors[x].getBlueF(), colors[x].getAlphaF());
      const Color dst(colors[y].getRedF(), colors[y].getGreenF(),
                      colors[y].getBlueF(), colors[y].getAlphaF());
      const Color expected = dst.Blend(src, BlendMode::kScreen);
      reference.DrawRect(
          rect, DlPaint()
                    .setColor(DlColor::RGBA(expected.red, expected.green,
                                            expected.blue, expected.alpha))
                    .setBlendMode(DlBlendMode::kSrc));
    }
  }
  auto texture = DisplayListToTexture(blended.Build(), kSize, renderer);
  ASSERT_TRUE(texture);
  EXPECT_TRUE(cache->labels.empty());
  auto expected = DisplayListToTexture(reference.Build(), kSize, renderer);
  ASSERT_TRUE(expected);
  const auto pixels = ReadBackdropPixels(GetContext(), texture);
  const auto expected_pixels = ReadBackdropPixels(GetContext(), expected);
  ASSERT_EQ(pixels.size(), static_cast<size_t>(kSize.Area() * 4));
  ASSERT_EQ(pixels.size(), expected_pixels.size());
  for (size_t i = 0; i < pixels.size(); i++) {
    EXPECT_LE(std::abs(int(pixels[i]) - int(expected_pixels[i])), 1) << i;
  }
}

// Each sampled image contains a quantized premultiplied texel. Decode that
// exact texel for the CPU reference instead of assuming unquantized colors.
TEST_P(AiksTest, ScreenImageFiltersAndVerticesMatchCpuOracleWithoutOffscreen) {
  using namespace flutter;
  constexpr ISize kSize(100, 100);
  const std::array<DlColor, 5> colors = {
      DlColor(0x00000000), DlColor(0xFFFFFFFF), DlColor(0x2040BF80),
      DlColor(0xBFFF4000), DlColor(0x800080FF),
  };
  std::array<sk_sp<DlImageImpeller>, 5> images;
  std::array<Color, 5> texel_colors;
  for (size_t y = 0; y < colors.size(); y++) {
    const auto color = colors[y];
    const Color premultiplied = Color(color.getRedF(), color.getGreenF(),
                                      color.getBlueF(), color.getAlphaF())
                                    .Premultiply();
    const std::array<uint8_t, 4> texel = {
        static_cast<uint8_t>(std::lround(premultiplied.red * 255)),
        static_cast<uint8_t>(std::lround(premultiplied.green * 255)),
        static_cast<uint8_t>(std::lround(premultiplied.blue * 255)),
        static_cast<uint8_t>(std::lround(premultiplied.alpha * 255)),
    };
    texel_colors[y] = Color::MakeRGBA8(texel[0], texel[1], texel[2], texel[3])
                          .Unpremultiply();
    std::vector<uint8_t> bytes(20 * 20 * 4);
    for (size_t offset = 0; offset < bytes.size(); offset += 4) {
      std::copy(texel.begin(), texel.end(), bytes.begin() + offset);
    }
    TextureDescriptor descriptor;
    descriptor.storage_mode = StorageMode::kDevicePrivate;
    descriptor.size = ISize(20, 20);
    descriptor.format = PixelFormat::kR8G8B8A8UNormInt;
    descriptor.usage = TextureUsage::kShaderRead;
    auto texture =
        GetContext()->GetResourceAllocator()->CreateTexture(descriptor);
    ASSERT_TRUE(texture);
    ASSERT_TRUE(texture->SetContents(bytes.data(), bytes.size()));
    images[y] = DlImageImpeller::Make(std::move(texture));
  }

  auto cache = std::make_shared<LabelRecordingCache>(
      GetContext()->GetResourceAllocator());
  AiksContext renderer(GetContext(), nullptr, cache);
  enum class Draw { kImage, kImageRect, kVertices };
  for (const auto draw : {Draw::kImage, Draw::kImageRect, Draw::kVertices}) {
    SCOPED_TRACE(static_cast<int>(draw));
    DisplayListBuilder blended;
    DisplayListBuilder reference;
    for (size_t y = 0; y < colors.size(); y++) {
      for (size_t x = 0; x < colors.size(); x++) {
        const auto rect = DlRect::MakeXYWH(x * 20, y * 20, 20, 20);
        const auto source = colors[x];
        DlPaint paint;
        paint.setBlendMode(DlBlendMode::kSrc);
        if (draw == Draw::kVertices) {
          const std::array<DlPoint, 4> positions = {
              rect.GetLeftTop(), rect.GetLeftBottom(), rect.GetRightTop(),
              rect.GetRightBottom()};
          const std::array<DlPoint, 4> uv = {DlPoint(0, 0), DlPoint(0, 20),
                                             DlPoint(20, 0), DlPoint(20, 20)};
          const std::array<DlColor, 4> vertex_colors = {source, source, source,
                                                        source};
          auto vertices = DlVertices::Make(DlVertexMode::kTriangleStrip, 4,
                                           positions.data(), uv.data(),
                                           vertex_colors.data());
          ASSERT_TRUE(vertices);
          paint.setColorSource(DlColorSource::MakeImage(
              images[y], DlTileMode::kClamp, DlTileMode::kClamp));
          blended.DrawVertices(vertices, DlBlendMode::kScreen, paint);
        } else {
          paint.setColorFilter(
              DlColorFilter::MakeBlend(source, DlBlendMode::kScreen));
          if (draw == Draw::kImage) {
            blended.DrawImage(images[y], rect.GetLeftTop(), {}, &paint);
          } else {
            blended.DrawImageRect(images[y], DlRect::MakeWH(20, 20), rect, {},
                                  &paint);
          }
        }
        const Color src(source.getRedF(), source.getGreenF(), source.getBlueF(),
                        source.getAlphaF());
        const Color expected = texel_colors[y].Blend(src, BlendMode::kScreen);
        reference.DrawRect(
            rect, DlPaint()
                      .setColor(DlColor::RGBA(expected.red, expected.green,
                                              expected.blue, expected.alpha))
                      .setBlendMode(DlBlendMode::kSrc));
      }
    }
    cache->labels.clear();
    auto texture = DisplayListToTexture(blended.Build(), kSize, renderer);
    ASSERT_TRUE(texture);
    EXPECT_TRUE(cache->labels.empty()) << "Screen must not allocate a snapshot";
    auto expected = DisplayListToTexture(reference.Build(), kSize, renderer);
    ASSERT_TRUE(expected);
    const auto pixels = ReadBackdropPixels(GetContext(), texture);
    const auto expected_pixels = ReadBackdropPixels(GetContext(), expected);
    ASSERT_EQ(pixels.size(), static_cast<size_t>(kSize.Area() * 4));
    ASSERT_EQ(pixels.size(), expected_pixels.size());
    for (size_t i = 0; i < pixels.size(); i++) {
      EXPECT_LE(std::abs(int(pixels[i]) - int(expected_pixels[i])), 1) << i;
    }
  }
}

class ScreenVulkanComparisonTest : public AiksTest {};
INSTANTIATE_VULKAN_PLAYGROUND_SUITE(ScreenVulkanComparisonTest);

// Comparison golden, not a look-approval test: per-sample fixed blending is
// observably different from the old fetch shader's averaged destination.
// The paired images and recorded deltas feed the explicit look gate in the DN.
TEST_P(ScreenVulkanComparisonTest,
       ScreenPreviousFetchAndPipelineClippedEdgesGolden) {
  using namespace flutter;
  ASSERT_TRUE(GetContext()->GetCapabilities()->SupportsFramebufferFetch());
  ASSERT_TRUE(GetContext()->GetCapabilities()->SupportsOffscreenMSAA());
  auto cache = std::make_shared<LabelRecordingCache>(
      GetContext()->GetResourceAllocator());
  AiksContext renderer(GetContext(), nullptr, cache);
  RenderTargetAllocator output(GetContext()->GetResourceAllocator());
  SetWindowSize(ISize(520, 932));
  DisplayListBuilder golden;
  Scalar golden_y = 0;
  for (const bool dark : {false, true}) {
    for (const Scalar scale : {1.0f, 1.25f, 2.0f}) {
      SCOPED_TRACE(dark);
      SCOPED_TRACE(scale);
      const ISize size(static_cast<int>(128 * scale),
                       static_cast<int>(104 * scale));
      auto render = [&](bool previous, bool mask) {
        auto target = output.CreateOffscreenMSAA(*GetContext(), size, 1);
        if (!target.IsValid() ||
            target.GetSampleCount() != SampleCount::kCount4) {
          return std::shared_ptr<Texture>();
        }
        Canvas canvas(renderer.GetContentContext(), target, false, false);
        FillRectGeometry geometry(Rect::MakeWH(128, 104));
        canvas.Scale(Vector2(scale, scale));
        canvas.DrawPaint({.color = mask   ? Color::BlackTransparent()
                                   : dark ? Color(0.06, 0.06, 0.06, 1)
                                          : Color(0.9, 0.9, 0.9, 1)});
        canvas.Save(/*total_content_depth=*/mask ? 1u : 3u);
        canvas.ClipGeometry(
            RoundRectGeometry(Rect::MakeLTRB(8.25, 8.5, 119.75, 95.5),
                              Size(22, 22)),
            Entity::ClipOperation::kIntersect, true);
        if (mask) {
          canvas.DrawPaint({.color = Color::White()});
        } else {
          canvas.DrawPaint({.color = dark ? Color(0.7, 0.7, 0.7, 1)
                                          : Color(0.15, 0.15, 0.15, 1)});
          canvas.DrawRect(Rect::MakeLTRB(0, 0, 64.375, 104),
                          {.color = dark ? Color(0.2, 0.2, 0.2, 1)
                                         : Color(0.65, 0.65, 0.65, 1)});
          auto gradient = std::make_shared<LinearGradientContents>(&geometry);
          gradient->SetEndPoints(Point(0, 0), Point(128, 104));
          gradient->SetColors(
              {Color(0.05, 0.3, 0.65, 0.35), Color(0.4, 0.1, 0.15, 0.25)});
          gradient->SetStops({0, 1});
          gradient->SetTileMode(Entity::TileMode::kClamp);
          CanvasBlendTestPeer::DrawScreen(canvas, gradient, previous);
        }
        canvas.Restore();
        const bool ok = canvas.EndReplay();
        renderer.GetContentContext().ResetTransientsBuffers();
        return ok ? target.GetRenderTargetTexture() : nullptr;
      };
      cache->labels.clear();
      auto previous = render(true, false);
      ASSERT_TRUE(previous);
      EXPECT_NE(std::find(cache->labels.begin(), cache->labels.end(),
                          "FramebufferBlendContents Snapshot"),
                cache->labels.end());
      cache->labels.clear();
      auto pipeline = render(false, false);
      ASSERT_TRUE(pipeline);
      EXPECT_TRUE(cache->labels.empty());
      auto mask = render(false, true);
      ASSERT_TRUE(mask);
      const auto previous_pixels = ReadBackdropPixels(GetContext(), previous);
      const auto pipeline_pixels = ReadBackdropPixels(GetContext(), pipeline);
      const auto mask_pixels = ReadBackdropPixels(GetContext(), mask);
      ASSERT_EQ(previous_pixels.size(), static_cast<size_t>(size.Area() * 4));
      ASSERT_EQ(previous_pixels.size(), pipeline_pixels.size());
      ASSERT_EQ(previous_pixels.size(), mask_pixels.size());
      int max_edge_delta = 0;
      int max_interior_delta = 0;
      int max_uncovered_delta = 0;
      size_t edge_pixels = 0;
      size_t changed_edge_pixels = 0;
      for (size_t offset = 0; offset < previous_pixels.size(); offset += 4) {
        const bool edge =
            mask_pixels[offset + 3] > 0 && mask_pixels[offset + 3] < 255;
        int delta = 0;
        for (size_t channel = 0; channel < 4; channel++) {
          delta =
              std::max(delta, std::abs(int(previous_pixels[offset + channel]) -
                                       int(pipeline_pixels[offset + channel])));
        }
        if (edge) {
          edge_pixels++;
          changed_edge_pixels += delta > 1;
          max_edge_delta = std::max(max_edge_delta, delta);
        } else if (mask_pixels[offset + 3] == 255) {
          max_interior_delta = std::max(max_interior_delta, delta);
        } else {
          max_uncovered_delta = std::max(max_uncovered_delta, delta);
        }
      }
      EXPECT_GT(edge_pixels, 0u)
          << "the fixture must exercise real 4x clip edges";
      EXPECT_GT(changed_edge_pixels, 0u)
          << "the old averaged-fetch path must not masquerade as per-sample "
             "Screen";
      const std::string prefix = std::string(dark ? "Dark" : "Light") +
                                 std::to_string(static_cast<int>(scale * 100));
      RecordProperty(prefix + "MaxEdgeDelta", max_edge_delta);
      RecordProperty(prefix + "MaxInteriorDelta", max_interior_delta);
      RecordProperty(prefix + "MaxUncoveredDelta", max_uncovered_delta);
      EXPECT_EQ(max_uncovered_delta, 0);
      golden.DrawImage(DlImageImpeller::Make(previous), DlPoint(0, golden_y),
                       {});
      golden.DrawImage(DlImageImpeller::Make(pipeline),
                       DlPoint(size.width + 8, golden_y), {});
      golden_y += size.height + 8;
    }
  }
  ASSERT_TRUE(OpenPlaygroundHere(golden.Build()));
}

}  // namespace impeller::testing
