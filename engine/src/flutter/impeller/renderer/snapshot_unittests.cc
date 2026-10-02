// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/snapshot.h"

#include "gtest/gtest.h"
#include "impeller/renderer/render_target.h"

namespace impeller {
namespace {

// CPU fixture exposes a real physical descriptor; these tests exercise no GPU.
class SnapshotTexture final : public Texture {
 public:
  explicit SnapshotTexture(ISize size) : Texture(Descriptor(size)) {}
  void SetLabel(std::string_view) override {}
  void SetLabel(std::string_view, std::string_view) override {}
  bool IsValid() const override { return true; }
  ISize GetSize() const override { return GetTextureDescriptor().size; }

 protected:
  bool OnSetContents(const uint8_t*, size_t, size_t) override { return false; }
  bool OnSetContents(std::shared_ptr<const fml::Mapping>, size_t) override {
    return false;
  }

 private:
  static TextureDescriptor Descriptor(ISize size) {
    TextureDescriptor descriptor;
    descriptor.size = size;
    descriptor.format = PixelFormat::kR8G8B8A8UNormInt;
    descriptor.storage_mode = StorageMode::kDevicePrivate;
    return descriptor;
  }
};

TEST(SnapshotTest, LegacyFullTextureCoverageAndUVsRemainPhysical) {
  auto texture = std::make_shared<SnapshotTexture>(ISize(128, 256));
  Snapshot snapshot{.texture = texture,
                    .transform = Matrix::MakeTranslation({100, 200}) *
                                 Matrix::MakeScale(Vector2(2, 3))};
  EXPECT_EQ(snapshot.GetTextureRect(), Rect::MakeSize(ISize(128, 256)));
  auto coverage = snapshot.GetCoverage();
  ASSERT_TRUE(coverage);
  EXPECT_EQ(*coverage, Rect::MakeXYWH(100, 200, 256, 768));
  auto uvs = snapshot.GetCoverageUVs(*coverage);
  ASSERT_TRUE(uvs);
  EXPECT_EQ((*uvs)[0], Point(0, 0));
  EXPECT_EQ((*uvs)[3], Point(1, 1));
  EXPECT_TRUE(snapshot.ShouldRasterizeForRuntimeEffects());
}

TEST(SnapshotTest, LogicalCropKeepsPhysicalTexelsAndUVNormalization) {
  auto texture = std::make_shared<SnapshotTexture>(ISize(128, 256));
  Snapshot snapshot{.texture = texture,
                    .transform = Matrix::MakeTranslation({100, 200}),
                    .texture_rect = Rect::MakeXYWH(16, 32, 48, 64)};
  EXPECT_EQ(texture->GetSize(), ISize(128, 256));
  auto coverage = snapshot.GetCoverage();
  ASSERT_TRUE(coverage);
  EXPECT_EQ(*coverage, Rect::MakeXYWH(116, 232, 48, 64));
  auto uvs = snapshot.GetCoverageUVs(*coverage);
  ASSERT_TRUE(uvs);
  EXPECT_EQ((*uvs)[0], Point(0.125, 0.125));
  EXPECT_EQ((*uvs)[3], Point(0.5, 0.375));
  EXPECT_TRUE(snapshot.ShouldRasterizeForRuntimeEffects());
}

TEST(SnapshotTest, TargetContentOriginAndLeaseSurviveSnapshotCopies) {
  auto texture = std::make_shared<SnapshotTexture>(ISize(128, 256));
  size_t released = 0;
  struct Lease {
    size_t& released;
    ~Lease() { released++; }
  };
  auto owner = std::make_shared<Lease>(released);
  RenderTarget target;
  ColorAttachment color;
  color.texture = texture;
  target.SetColorAttachment(color, 0);
  ASSERT_TRUE(target.SetContentRect(IRect::MakeXYWH(16, 32, 48, 64)));
  target.SetResourceOwner(owner);
  auto snapshot =
      Snapshot::FromRenderTarget(target, Matrix::MakeTranslation({100, 200}));
  auto copy = snapshot;
  EXPECT_EQ(snapshot.GetCoverage(), Rect::MakeXYWH(100, 200, 48, 64));
  EXPECT_EQ(snapshot.GetTextureRect(), Rect::MakeXYWH(16, 32, 48, 64));
  EXPECT_EQ(target.GetRenderTargetSize(), ISize(128, 256));
  EXPECT_FALSE(target.SetContentRect(IRect::MakeXYWH(100, 200, 48, 64)));
  owner.reset();
  target.SetResourceOwner(nullptr);
  snapshot.resource_owner.reset();
  EXPECT_EQ(released, 0u);
  copy.resource_owner.reset();
  EXPECT_EQ(released, 1u);
}

TEST(SnapshotTest, InvalidLogicalRectsHaveNoReadableCoverageOrUVs) {
  Snapshot snapshot{
      .texture = std::make_shared<SnapshotTexture>(ISize(128, 256)),
      .texture_rect = Rect::MakeXYWH(100, 200, 48, 64)};
  EXPECT_FALSE(snapshot.GetCoverage());
  EXPECT_FALSE(snapshot.GetUVTransform());
  snapshot.texture_rect = Rect();
  EXPECT_FALSE(snapshot.GetCoverage());
  EXPECT_FALSE(snapshot.GetUVTransform());
  snapshot.texture.reset();
  EXPECT_FALSE(snapshot.GetCoverage());
  EXPECT_FALSE(snapshot.GetUVTransform());
}

TEST(SnapshotTest, CapturedOpaqueEvidenceFollowsImmutableOwnerAndExactCrop) {
  Snapshot snapshot{
      .texture = std::make_shared<SnapshotTexture>(ISize(128, 256)),
      .texture_rect = Rect::MakeXYWH(16, 32, 48, 64),
      .resource_owner = std::make_shared<int>(17),
      .is_immutable_captured_backdrop = true,
      .captured_opaque_texels = Rect::MakeXYWH(8, 24, 48, 64)};
  auto copy = snapshot;
  EXPECT_EQ(copy.GetCapturedOpaqueRect(), Rect::MakeXYWH(16, 32, 40, 56));
  snapshot.resource_owner.reset();
  EXPECT_FALSE(snapshot.GetCapturedOpaqueRect());
  EXPECT_TRUE(copy.GetCapturedOpaqueRect());
  copy.opacity = .999f;
  EXPECT_FALSE(copy.GetCapturedOpaqueRect());
  copy.opacity = 1.f;
  copy.texture_rect = Rect::MakeXYWH(80, 160, 32, 32);
  EXPECT_FALSE(copy.GetCapturedOpaqueRect());
}

TEST(SnapshotTest, GenericTargetAndOpaqueFormatDoNotCreateCaptureEvidence) {
  auto texture = std::make_shared<SnapshotTexture>(ISize(128, 256));
  RenderTarget target;
  ColorAttachment color;
  color.texture = texture;
  color.clear_color = Color::White();
  target.SetColorAttachment(color, 0u);
  target.SetResourceOwner(std::make_shared<int>(17));
  EXPECT_FALSE(Snapshot::FromRenderTarget(target).GetCapturedOpaqueRect());
}

TEST(SnapshotTest, ImmutableFilterSourceAndOpaqueAlphaAreIndependentFacts) {
  Snapshot snapshot{
      .texture = std::make_shared<SnapshotTexture>(ISize(128, 256)),
      .resource_owner = std::make_shared<int>(17),
      .is_immutable_captured_backdrop = true};
  EXPECT_TRUE(snapshot.IsImmutableCapturedBackdrop());
  EXPECT_FALSE(snapshot.GetCapturedOpaqueRect());
  snapshot.resource_owner.reset();
  EXPECT_FALSE(snapshot.IsImmutableCapturedBackdrop());
}

}  // namespace
}  // namespace impeller
