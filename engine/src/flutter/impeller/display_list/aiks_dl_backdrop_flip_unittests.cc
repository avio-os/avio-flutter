// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A backdrop filter inside another layer flips that layer's pass target
// (Avio patch 48). On backends that cannot read from a resolve texture
// (Vulkan) the flip swaps in a lazily allocated single-sample secondary.

#include <algorithm>
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
#include "display_list/effects/dl_color_filter.h"
#include "display_list/effects/dl_image_filter.h"
#include "fml/synchronization/count_down_latch.h"
#include "impeller/core/device_buffer.h"
#include "impeller/display_list/aiks_context.h"
#include "impeller/display_list/aiks_unittests.h"
#include "impeller/display_list/dl_dispatcher.h"
#include "impeller/entity/render_target_cache.h"

namespace impeller::testing {
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

// Records the label of every single-sample offscreen request.
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

}  // namespace impeller::testing
