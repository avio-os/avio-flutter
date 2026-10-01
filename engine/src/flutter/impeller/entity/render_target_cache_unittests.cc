// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

#include "flutter/testing/testing.h"
#include "impeller/base/validation.h"
#include "impeller/core/allocator.h"
#include "impeller/core/formats.h"
#include "impeller/core/texture_descriptor.h"
#include "impeller/entity/entity_playground.h"
#include "impeller/entity/render_target_cache.h"
#include "impeller/playground/playground_test.h"
#include "impeller/renderer/capabilities.h"
#include "impeller/renderer/render_resource_usage.h"
#include "impeller/renderer/testing/mocks.h"

namespace impeller {
namespace testing {

using RenderTargetCacheTest = EntityPlayground;
INSTANTIATE_PLAYGROUND_SUITE(RenderTargetCacheTest);

class TestAllocator : public Allocator {
 public:
  TestAllocator() = default;

  ~TestAllocator() = default;

  ISize GetMaxTextureSizeSupported() const override {
    return ISize(1024, 1024);
  };

  std::shared_ptr<DeviceBuffer> OnCreateBuffer(
      const DeviceBufferDescriptor& desc) override {
    if (should_fail) {
      return nullptr;
    }
    return std::make_shared<MockDeviceBuffer>(desc);
  };

  virtual std::shared_ptr<Texture> OnCreateTexture(
      const TextureDescriptor& desc,
      bool threadsafe) override {
    if (should_fail) {
      return nullptr;
    }
    return std::make_shared<MockTexture>(desc);
  };

  bool should_fail = false;
};

TEST_P(RenderTargetCacheTest, CachesUsedTexturesAcrossFrames) {
  auto render_target_cache = RenderTargetCache(
      GetContext()->GetResourceAllocator(), /*keep_alive_frame_count=*/0);

  render_target_cache.Start();
  // Create two render targets of the same exact size/shape. Both should be
  // marked as used this frame, so the cached data set will contain two.
  render_target_cache.CreateOffscreen(*GetContext(), {100, 100}, 1);
  render_target_cache.CreateOffscreen(*GetContext(), {100, 100}, 1);

  EXPECT_EQ(render_target_cache.CachedTextureCount(), 2u);

  render_target_cache.End();
  render_target_cache.Start();

  // Next frame, only create one texture. The set will still contain two,
  // but one will be removed at the end of the frame.
  render_target_cache.CreateOffscreen(*GetContext(), {100, 100}, 1);
  EXPECT_EQ(render_target_cache.CachedTextureCount(), 2u);

  render_target_cache.End();
  EXPECT_EQ(render_target_cache.CachedTextureCount(), 1u);
}

TEST_P(RenderTargetCacheTest, CachesUsedTexturesAcrossFramesWithKeepAlive) {
  auto render_target_cache = RenderTargetCache(
      GetContext()->GetResourceAllocator(), /*keep_alive_frame_count=*/3);

  render_target_cache.Start();
  // Create two render targets of the same exact size/shape. Both should be
  // marked as used this frame, so the cached data set will contain two.
  render_target_cache.CreateOffscreen(*GetContext(), {100, 100}, 1);
  render_target_cache.CreateOffscreen(*GetContext(), {100, 100}, 1);

  EXPECT_EQ(render_target_cache.CachedTextureCount(), 2u);

  render_target_cache.End();
  render_target_cache.Start();

  // The unused texture is kept alive until the keep alive countdown
  // reaches 0.
  EXPECT_EQ(render_target_cache.CachedTextureCount(), 2u);

  for (auto i = 0; i < 3; i++) {
    render_target_cache.Start();
    render_target_cache.End();
    EXPECT_EQ(render_target_cache.CachedTextureCount(), 2u);
  }
  // After the countdown has elapsed the texture is removed.
  render_target_cache.Start();
  render_target_cache.End();
  EXPECT_EQ(render_target_cache.CachedTextureCount(), 0u);
}

TEST_P(RenderTargetCacheTest, DoesNotPersistFailedAllocations) {
  ScopedValidationDisable disable;
  auto allocator = std::make_shared<TestAllocator>();
  auto render_target_cache =
      RenderTargetCache(allocator, /*keep_alive_frame_count=*/0);

  render_target_cache.Start();
  allocator->should_fail = true;

  auto render_target =
      render_target_cache.CreateOffscreen(*GetContext(), {100, 100}, 1);

  EXPECT_FALSE(render_target.IsValid());
  EXPECT_EQ(render_target_cache.CachedTextureCount(), 0u);
}

TEST_P(RenderTargetCacheTest, CachedTextureGetsNewAttachmentConfig) {
  auto render_target_cache = RenderTargetCache(
      GetContext()->GetResourceAllocator(), /*keep_alive_frame_count=*/0);

  render_target_cache.Start();
  RenderTarget::AttachmentConfig color_attachment_config =
      RenderTarget::kDefaultColorAttachmentConfig;
  RenderTarget target1 = render_target_cache.CreateOffscreen(
      *GetContext(), {100, 100}, 1, "Offscreen1", color_attachment_config);
  render_target_cache.End();

  render_target_cache.Start();
  color_attachment_config.clear_color = Color::Red();
  RenderTarget target2 = render_target_cache.CreateOffscreen(
      *GetContext(), {100, 100}, 1, "Offscreen2", color_attachment_config);
  render_target_cache.End();

  ColorAttachment color1 = target1.GetColorAttachment(0);
  ColorAttachment color2 = target2.GetColorAttachment(0);
  // The second color attachment should reuse the first attachment's texture
  // but with attributes from the second AttachmentConfig.
  EXPECT_EQ(color2.texture, color1.texture);
  EXPECT_EQ(color2.clear_color, Color::Red());
}

TEST_P(RenderTargetCacheTest, CreateWithEmptySize) {
  auto render_target_cache = RenderTargetCache(
      GetContext()->GetResourceAllocator(), /*keep_alive_frame_count=*/0);

  render_target_cache.Start();
  RenderTarget empty_target =
      render_target_cache.CreateOffscreen(*GetContext(), {100, 0}, 1);
  RenderTarget empty_target_msaa =
      render_target_cache.CreateOffscreenMSAA(*GetContext(), {0, 0}, 1);
  render_target_cache.End();

  {
    ScopedValidationDisable disable_validation;
    EXPECT_FALSE(empty_target.IsValid());
    EXPECT_FALSE(empty_target_msaa.IsValid());
  }
}

// Non-playground fixture: a fake allocator whose textures report a page-rounded
// backend allocation, a mock context with real capabilities, and an injected
// clock. These tests need no GPU.
namespace {

class FakeTexture final : public Texture {
 public:
  FakeTexture(const TextureDescriptor& desc, size_t allocated_bytes)
      : Texture(desc), allocated_bytes_(allocated_bytes) {}

  void SetLabel(std::string_view label) override {}
  void SetLabel(std::string_view label, std::string_view trailing) override {}
  bool IsValid() const override { return true; }
  ISize GetSize() const override { return GetTextureDescriptor().size; }
  size_t GetAllocatedByteSize() const override { return allocated_bytes_; }

 private:
  bool OnSetContents(const uint8_t* contents,
                     size_t length,
                     size_t slice) override {
    return true;
  }
  bool OnSetContents(std::shared_ptr<const fml::Mapping> mapping,
                     size_t slice) override {
    return true;
  }

  const size_t allocated_bytes_;
};

class FakeAllocator final : public Allocator {
 public:
  ISize GetMaxTextureSizeSupported() const override {
    return ISize(8192, 8192);
  }

  std::shared_ptr<DeviceBuffer> OnCreateBuffer(
      const DeviceBufferDescriptor& desc) override {
    return std::make_shared<MockDeviceBuffer>(desc);
  }

  std::shared_ptr<Texture> OnCreateTexture(const TextureDescriptor& desc,
                                           bool threadsafe) override {
    created_textures++;
    return std::make_shared<FakeTexture>(desc, AllocatedBytesFor(desc));
  }

  // A backend allocation rounds each image up to whole pages.
  static size_t AllocatedBytesFor(const TextureDescriptor& desc) {
    const size_t texel_bytes = desc.GetByteSizeOfAllMipLevels() *
                               static_cast<size_t>(desc.sample_count);
    return (texel_bytes + 4095u) / 4096u * 4096u;
  }

  size_t created_textures = 0u;
};

class RenderTargetCacheResourceTest : public ::testing::Test {
 protected:
  RenderTargetCacheResourceTest()
      : capabilities_(
            CapabilitiesBuilder()
                .SetDefaultColorFormat(PixelFormat::kR8G8B8A8UNormInt)
                .SetDefaultStencilFormat(PixelFormat::kS8UInt)
                .SetDefaultDepthStencilFormat(PixelFormat::kD24UnormS8Uint)
                .SetSupportsOffscreenMSAA(true)
                .Build()) {
    ON_CALL(context_, GetCapabilities())
        .WillByDefault(::testing::ReturnRef(capabilities_));
  }

  void UseTestClock(RenderTargetCache& cache) {
    cache.SetClockForTesting([this] { return now_; });
  }

  // Expected nominal and real bytes of every distinct attached texture.
  static std::pair<size_t, size_t> BytesOf(const RenderTarget& target) {
    std::vector<const Texture*> seen;
    size_t nominal = 0u;
    size_t real = 0u;
    target.IterateAllAttachments([&](const Attachment& attachment) -> bool {
      for (const auto& texture :
           {attachment.texture, attachment.resolve_texture}) {
        if (!texture ||
            std::find(seen.begin(), seen.end(), texture.get()) != seen.end()) {
          continue;
        }
        seen.push_back(texture.get());
        const auto& desc = texture->GetTextureDescriptor();
        nominal += desc.GetByteSizeOfAllMipLevels() *
                   static_cast<size_t>(desc.sample_count);
        real += FakeAllocator::AllocatedBytesFor(desc);
      }
      return true;
    });
    return {nominal, real};
  }

  std::shared_ptr<const Capabilities> capabilities_;
  ::testing::NiceMock<MockImpellerContext> context_;
  std::shared_ptr<FakeAllocator> allocator_ = std::make_shared<FakeAllocator>();
  std::chrono::steady_clock::time_point now_ =
      std::chrono::steady_clock::time_point() + std::chrono::hours(1);
};

}  // namespace

// Red before EN46: there was no idle release between frames.
TEST_F(RenderTargetCacheResourceTest, ReleaseIdleDropsOld) {
  RenderTargetCache cache(allocator_);
  UseTestClock(cache);
  cache.Start();
  ASSERT_TRUE(cache.CreateOffscreen(context_, {100, 100}, 1).IsValid());
  cache.End();
  ASSERT_EQ(cache.CachedTextureCount(), 1u);

  now_ += kIdleReleaseMinUnused + std::chrono::milliseconds(1);
  const auto result = cache.ReleaseIdle(kIdleReleaseMinUnused);
  EXPECT_EQ(result.before.entries, 1u);
  EXPECT_EQ(result.after, ResourceCacheUsage{});
  EXPECT_EQ(result.kept_in_use, 0u);
  EXPECT_EQ(result.kept_recent, 0u);
  EXPECT_EQ(cache.CachedTextureCount(), 0u);
}

TEST_F(RenderTargetCacheResourceTest, ReleaseIdleKeepsRecent) {
  RenderTargetCache cache(allocator_);
  UseTestClock(cache);
  cache.Start();
  ASSERT_TRUE(cache.CreateOffscreen(context_, {100, 100}, 1).IsValid());
  cache.End();

  now_ += kIdleReleaseMinUnused - std::chrono::milliseconds(1);
  const auto result = cache.ReleaseIdle(kIdleReleaseMinUnused);
  EXPECT_EQ(result.kept_recent, 1u);
  EXPECT_EQ(result.after, result.before);
  EXPECT_EQ(cache.CachedTextureCount(), 1u);
}

// An entry leased by an open workload survives any release.
TEST_F(RenderTargetCacheResourceTest, ReleaseIdleInOpenScopeKeepsLeased) {
  RenderTargetCache cache(allocator_);
  UseTestClock(cache);
  cache.Start();
  ASSERT_TRUE(cache.CreateOffscreen(context_, {100, 100}, 1).IsValid());

  now_ += kIdleReleaseMinUnused * 2;
  const auto result = cache.ReleaseIdle(kIdleReleaseMinUnused);
  EXPECT_EQ(result.kept_in_use, 1u);
  EXPECT_EQ(cache.CachedTextureCount(), 1u);
  cache.End();
}

TEST_F(RenderTargetCacheResourceTest, ReleaseIdleReportsExactBytes) {
  RenderTargetCache cache(allocator_);
  UseTestClock(cache);
  cache.Start();
  const RenderTarget target = cache.CreateOffscreenMSAA(context_, {100, 60}, 1);
  ASSERT_TRUE(target.IsValid());
  cache.End();
  const auto [nominal, real] = BytesOf(target);
  ASSERT_GT(nominal, 0u);
  ASSERT_GT(real, nominal);

  const RenderResourceUsage report = cache.ReportUsage(false);
  EXPECT_EQ(report.entries, 1u);
  EXPECT_EQ(report.nominal_bytes, nominal);
  EXPECT_EQ(report.real_bytes, real);
  EXPECT_EQ(report.distinct_keys, 1u);
  EXPECT_EQ(report.duplicate_entries, 0u);
  EXPECT_EQ(report.leased_entries, 0u);
  EXPECT_EQ(report.peak_leased_nominal_bytes, nominal);
  EXPECT_EQ(report.created_entries, 1u);
  EXPECT_EQ(report.created_real_bytes, real);
  EXPECT_EQ(report.orphans_released_entries, 0u);

  now_ += kIdleReleaseMinUnused;
  const auto result = cache.ReleaseIdle(kIdleReleaseMinUnused);
  EXPECT_EQ(result.before.bytes, nominal);
  EXPECT_EQ(result.after, ResourceCacheUsage{});

  // The creation is still reported for its interval after the release, and
  // a new interval starts empty.
  const RenderResourceUsage interval = cache.ReportUsage(true);
  EXPECT_EQ(interval.entries, 0u);
  EXPECT_EQ(interval.created_entries, 1u);
  EXPECT_EQ(interval.created_real_bytes, real);
  EXPECT_EQ(cache.ReportUsage(false), RenderResourceUsage{});
}

}  // namespace testing
}  // namespace impeller
