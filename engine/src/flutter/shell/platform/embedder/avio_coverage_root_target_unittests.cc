// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if IMPELLER_SUPPORTS_RENDERING
#include "flutter/shell/platform/embedder/avio_coverage_root_target.h"

#include "gtest/gtest.h"

namespace flutter {
namespace testing {
namespace {

class CoverageImportedTexture final : public impeller::Texture {
 public:
  CoverageImportedTexture(impeller::SampleCount samples, bool valid)
      : Texture(Descriptor(samples)), valid_(valid) {}
  bool IsValid() const override { return valid_; }
  impeller::ISize GetSize() const override {
    return GetTextureDescriptor().size;
  }
  void SetLabel(std::string_view) override {}
  void SetLabel(std::string_view, std::string_view) override {}

 private:
  static impeller::TextureDescriptor Descriptor(impeller::SampleCount samples) {
    impeller::TextureDescriptor descriptor;
    descriptor.size = {32, 24};
    descriptor.format = impeller::PixelFormat::kR8G8B8A8UNormInt;
    descriptor.storage_mode = impeller::StorageMode::kDevicePrivate;
    descriptor.sample_count = samples;
    descriptor.usage = impeller::TextureUsage::kRenderTarget;
    return descriptor;
  }
  bool OnSetContents(const uint8_t*, size_t, size_t) override { return false; }
  bool OnSetContents(std::shared_ptr<const fml::Mapping>, size_t) override {
    return false;
  }
  const bool valid_;
};

TEST(EmbedderCoverageRootTargetTest,
     CoverageRootRetainsImportedColorWithoutAuxiliaryAttachments) {
  for (bool preserved : {false, true}) {
    auto texture = std::make_shared<CoverageImportedTexture>(
        impeller::SampleCount::kCount1, true);
    const std::weak_ptr<impeller::Texture> lifetime = texture;
    auto target = MakeAvioCoverageRootTarget(texture, preserved);
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->GetSampleCount(), impeller::SampleCount::kCount1);
    EXPECT_EQ(target->GetTotalAttachmentCount(), 1u);
    EXPECT_FALSE(target->GetDepthAttachment());
    EXPECT_FALSE(target->GetStencilAttachment());
    auto color = target->GetColorAttachment(0u);
    EXPECT_EQ(color.texture, texture);
    EXPECT_EQ(color.resolve_texture, nullptr);
    EXPECT_EQ(color.store_action, impeller::StoreAction::kStore);
    EXPECT_EQ(color.load_action, preserved ? impeller::LoadAction::kLoad
                                           : impeller::LoadAction::kClear);
    texture.reset();
    EXPECT_FALSE(lifetime.expired());
    target.reset();
    // The attachment copied for inspection still keeps the imported source.
    EXPECT_FALSE(lifetime.expired());
    color.texture.reset();
    EXPECT_TRUE(lifetime.expired());
  }
}

TEST(EmbedderCoverageRootTargetTest, CoverageRootRejectsInvalidImportedColor) {
  EXPECT_EQ(MakeAvioCoverageRootTarget(nullptr, true), nullptr);
  auto invalid = std::make_shared<CoverageImportedTexture>(
      impeller::SampleCount::kCount1, false);
  EXPECT_EQ(MakeAvioCoverageRootTarget(invalid, false), nullptr);
  auto multisampled = std::make_shared<CoverageImportedTexture>(
      impeller::SampleCount::kCount4, true);
  EXPECT_EQ(MakeAvioCoverageRootTarget(multisampled, true), nullptr);
}

}  // namespace
}  // namespace testing
}  // namespace flutter
#endif  // IMPELLER_SUPPORTS_RENDERING
