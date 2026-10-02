// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/testing/testing.h"
#include "gmock/gmock.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/contents/filters/blend_filter_contents.h"
#include "impeller/entity/entity_playground.h"

namespace impeller {
namespace testing {

TEST(BlendCoefficientsTest, ScreenShaderMatchesPremultipliedColorBlend) {
  const std::array<Color, 5> colors = {
      Color::BlackTransparent(),         Color::White(),
      Color(0.25f, 0.75f, 0.5f, 0.125f), Color(1.0f, 0.25f, 0.0f, 0.75f),
      Color(0.0f, 0.5f, 1.0f, 0.5f),
  };
  for (const bool supports_decal : {false, true}) {
    const auto constants = GetPorterDuffSpecConstants(supports_decal);
    const auto& c = constants[static_cast<size_t>(BlendMode::kScreen)];
    ASSERT_EQ(c.size(), 6u);
    EXPECT_EQ(c[0], supports_decal ? 1.0f : 0.0f);
    for (const auto& source : colors) {
      for (const auto& destination : colors) {
        const auto src = source.Premultiply();
        const auto dst = destination.Premultiply();
        const auto result =
            src * (c[1] + dst.alpha * c[2]) +
            dst * (Color(c[3], c[3], c[3], c[3]) +
                   Color(src.alpha, src.alpha, src.alpha, src.alpha) * c[4] +
                   src * c[5]);
        const auto expected =
            destination.Blend(source, BlendMode::kScreen).Premultiply();
        EXPECT_NEAR(result.red, expected.red, 1e-5f);
        EXPECT_NEAR(result.green, expected.green, 1e-5f);
        EXPECT_NEAR(result.blue, expected.blue, 1e-5f);
        EXPECT_NEAR(result.alpha, expected.alpha, 1e-5f);
      }
    }
  }
}

TEST(BlendCoefficientsTest, ScreenIsItsOwnInverseForVertexBlends) {
  EXPECT_EQ(InvertPorterDuffBlend(BlendMode::kScreen), BlendMode::kScreen);
  EXPECT_FALSE(InvertPorterDuffBlend(BlendMode::kOverlay).has_value());
}

class BlendFilterContentsTest : public EntityPlayground {
 public:
  /// Create a texture that has been cleared to transparent black.
  std::shared_ptr<Texture> MakeTexture(ISize size) {
    std::shared_ptr<CommandBuffer> command_buffer =
        GetContentContext().GetContext()->CreateCommandBuffer();
    if (!command_buffer) {
      return nullptr;
    }

    auto render_target = GetContentContext().MakeSubpass(
        "Clear Subpass", size, command_buffer,
        [](const ContentContext&, RenderPass&) { return true; });

    if (!GetContentContext()
             .GetContext()
             ->GetCommandQueue()
             ->Submit(/*buffers=*/{command_buffer})
             .ok()) {
      return nullptr;
    }

    if (render_target.ok()) {
      return render_target.value().GetRenderTargetTexture();
    }
    return nullptr;
  }
};
INSTANTIATE_PLAYGROUND_SUITE(BlendFilterContentsTest);

// https://github.com/flutter/flutter/issues/149216
TEST_P(BlendFilterContentsTest, AdvancedBlendColorAlignsColorTo4) {
  std::shared_ptr<Texture> texture = MakeTexture(ISize(100, 100));
  BlendFilterContents filter_contents;
  filter_contents.SetInputs({FilterInput::Make(texture)});
  filter_contents.SetForegroundColor(Color(1.0, 0.0, 0.0, 1.0));
  filter_contents.SetBlendMode(BlendMode::kColorDodge);

  ContentContext& renderer = GetContentContext();
  // Add random byte to get the HostBuffer in a bad alignment.
  uint8_t byte = 0xff;
  BufferView buffer_view = renderer.GetTransientsDataBuffer().Emplace(
      &byte, /*length=*/1, /*align=*/1);
  EXPECT_EQ(buffer_view.GetRange().offset, 4u);
  EXPECT_EQ(buffer_view.GetRange().length, 1u);
  Entity entity;

  std::optional<Entity> result = filter_contents.GetEntity(
      renderer, entity, /*coverage_hint=*/std::nullopt);

  EXPECT_TRUE(result.has_value());
}

}  // namespace testing
}  // namespace impeller
