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

// Red before EN47: the key ignored the pixel format, so a request for a
// 16-bit float target reused an 8-bit one.
TEST_F(RenderTargetCacheResourceTest, KeyDistinguishesPixelFormat) {
  RenderTargetCache cache(allocator_);
  cache.Start();
  const RenderTarget rgba8 = cache.CreateOffscreen(context_, {100, 100}, 1);
  ASSERT_TRUE(rgba8.IsValid());
  cache.End();

  cache.Start();
  const RenderTarget f16 =
      cache.CreateOffscreen(context_, {100, 100}, 1, "Offscreen",
                            RenderTarget::kDefaultColorAttachmentConfig,
                            RenderTarget::kDefaultStencilAttachmentConfig,
                            nullptr, nullptr, PixelFormat::kR16G16B16A16Float);
  ASSERT_TRUE(f16.IsValid());
  EXPECT_NE(f16.GetRenderTargetTexture(), rgba8.GetRenderTargetTexture());
  EXPECT_EQ(f16.GetRenderTargetPixelFormat(), PixelFormat::kR16G16B16A16Float);
  EXPECT_EQ(cache.GetLastMissReasonForTesting(),
            RenderTargetCache::MissReason::kNoEntry);
  cache.End();
  EXPECT_EQ(cache.CachedTextureCount(), 2u);

  // Each key still reuses its own entry.
  cache.Start();
  EXPECT_EQ(
      cache.CreateOffscreen(context_, {100, 100}, 1).GetRenderTargetTexture(),
      rgba8.GetRenderTargetTexture());
  cache.End();
}

// Red before EN47: the key ignored storage modes.
TEST_F(RenderTargetCacheResourceTest, KeyDistinguishesStorageModes) {
  RenderTargetCache cache(allocator_);
  cache.Start();
  const RenderTarget device_private =
      cache.CreateOffscreen(context_, {64, 64}, 1);
  ASSERT_TRUE(device_private.IsValid());
  cache.End();

  RenderTarget::AttachmentConfig host_visible =
      RenderTarget::kDefaultColorAttachmentConfig;
  host_visible.storage_mode = StorageMode::kHostVisible;
  cache.Start();
  const RenderTarget other =
      cache.CreateOffscreen(context_, {64, 64}, 1, "Offscreen", host_visible);
  ASSERT_TRUE(other.IsValid());
  EXPECT_NE(other.GetRenderTargetTexture(),
            device_private.GetRenderTargetTexture());
  EXPECT_EQ(other.GetRenderTargetTexture()->GetTextureDescriptor().storage_mode,
            StorageMode::kHostVisible);
  cache.End();

  // MSAA resolve storage is part of the key too.
  RenderTarget::AttachmentConfigMSAA msaa_config =
      RenderTarget::kDefaultColorAttachmentConfigMSAA;
  cache.Start();
  const RenderTarget first_msaa =
      cache.CreateOffscreenMSAA(context_, {64, 64}, 1, "MSAA", msaa_config);
  cache.End();
  msaa_config.resolve_storage_mode = StorageMode::kHostVisible;
  cache.Start();
  const RenderTarget second_msaa =
      cache.CreateOffscreenMSAA(context_, {64, 64}, 1, "MSAA", msaa_config);
  EXPECT_NE(second_msaa.GetRenderTargetTexture(),
            first_msaa.GetRenderTargetTexture());
  cache.End();
}

TEST_F(RenderTargetCacheResourceTest, MissReasonClassification) {
  RenderTargetCache cache(allocator_, /*keep_alive_frame_count=*/0);

  cache.Start();
  ASSERT_TRUE(cache.CreateOffscreen(context_, {100, 100}, 1).IsValid());
  EXPECT_EQ(cache.GetLastMissReasonForTesting(),
            RenderTargetCache::MissReason::kNoEntry);
  // The only entry of this key is leased by this frame.
  ASSERT_TRUE(cache.CreateOffscreen(context_, {100, 100}, 1).IsValid());
  EXPECT_EQ(cache.GetLastMissReasonForTesting(),
            RenderTargetCache::MissReason::kAllLeased);
  cache.End();

  // Both entries are unleased now; a different extent of the same kind.
  cache.Start();
  ASSERT_TRUE(cache.CreateOffscreen(context_, {100, 101}, 1).IsValid());
  EXPECT_EQ(cache.GetLastMissReasonForTesting(),
            RenderTargetCache::MissReason::kExtentMismatch);
  cache.End();
  // keep_alive 0: the two 100x100 entries were dropped at that End.
  ASSERT_EQ(cache.CachedTextureCount(), 1u);

  cache.Start();
  ASSERT_TRUE(cache.CreateOffscreen(context_, {100, 100}, 1).IsValid());
  EXPECT_EQ(cache.GetLastMissReasonForTesting(),
            RenderTargetCache::MissReason::kAgedOut);
  cache.End();

  cache.Start();
  cache.DisableCache();
  ASSERT_TRUE(cache.CreateOffscreen(context_, {100, 100}, 1).IsValid());
  EXPECT_EQ(cache.GetLastMissReasonForTesting(),
            RenderTargetCache::MissReason::kDisabled);
  cache.EnableCache();
  cache.End();
}

// Red before EN51 (the scope API did not exist; with per-Canvas aging the
// entry died after keep-alive foreign Canvases).
TEST_F(RenderTargetCacheResourceTest, ForeignCanvasesWithinOneFrameDoNotAge) {
  RenderTargetCache cache(allocator_);  // keep-alive 4
  const uint64_t first_view = cache.BeginScope();
  const RenderTarget status_bar = cache.CreateOffscreen(context_, {100, 30}, 1);
  ASSERT_TRUE(status_bar.IsValid());
  cache.EndScope(first_view);

  // Fourteen sibling views render in the same frame.
  for (int view = 0; view < 14; view++) {
    const uint64_t scope = cache.BeginScope();
    ASSERT_TRUE(cache.CreateOffscreen(context_, {200, 200}, 1).IsValid());
    cache.EndScope(scope);
  }
  const size_t created = allocator_->created_textures;

  const uint64_t again = cache.BeginScope();
  EXPECT_EQ(
      cache.CreateOffscreen(context_, {100, 30}, 1).GetRenderTargetTexture(),
      status_bar.GetRenderTargetTexture());
  cache.EndScope(again);
  EXPECT_EQ(allocator_->created_textures, created);
  cache.End();
}

TEST_F(RenderTargetCacheResourceTest, EntryUnusedForKeepAliveFramesIsDropped) {
  RenderTargetCache cache(allocator_, /*keep_alive_frame_count=*/4);
  const uint64_t scope = cache.BeginScope();
  ASSERT_TRUE(cache.CreateOffscreen(context_, {100, 100}, 1).IsValid());
  cache.EndScope(scope);
  cache.End();  // The frame that used it.

  for (int frame = 0; frame < 4; frame++) {
    cache.End();
    EXPECT_EQ(cache.CachedTextureCount(), 1u) << "frame " << frame;
  }
  cache.End();
  EXPECT_EQ(cache.CachedTextureCount(), 0u);
}

// Snapshot (toImage) Canvases lease and return; only frames age.
TEST_F(RenderTargetCacheResourceTest, ToImageCanvasDoesNotAge) {
  RenderTargetCache cache(allocator_, /*keep_alive_frame_count=*/0);
  const uint64_t view = cache.BeginScope();
  ASSERT_TRUE(cache.CreateOffscreen(context_, {100, 100}, 1).IsValid());
  cache.EndScope(view);
  for (int snapshot = 0; snapshot < 20; snapshot++) {
    const uint64_t scope = cache.BeginScope();
    cache.EndScope(scope);
  }
  EXPECT_EQ(cache.CachedTextureCount(), 1u);
}

// Red before EN51: a nested Canvas's Start released every lease, so it could
// be handed the texture its outer Canvas was still rendering into.
TEST_F(RenderTargetCacheResourceTest, NestedScopeKeepsOuterLeases) {
  RenderTargetCache cache(allocator_);
  const uint64_t outer = cache.BeginScope();
  const RenderTarget outer_target =
      cache.CreateOffscreen(context_, {100, 100}, 1);
  ASSERT_TRUE(outer_target.IsValid());

  const uint64_t inner = cache.BeginScope();
  const RenderTarget inner_target =
      cache.CreateOffscreen(context_, {100, 100}, 1);
  ASSERT_TRUE(inner_target.IsValid());
  EXPECT_NE(inner_target.GetRenderTargetTexture(),
            outer_target.GetRenderTargetTexture());
  cache.EndScope(inner);

  // Back in the outer scope: the inner texture is free again, the outer one
  // is still leased.
  const RenderTarget reused = cache.CreateOffscreen(context_, {100, 100}, 1);
  EXPECT_EQ(reused.GetRenderTargetTexture(),
            inner_target.GetRenderTargetTexture());
  cache.EndScope(outer);
  cache.End();
  EXPECT_EQ(cache.CachedTextureCount(), 2u);
}

// The disable count belongs to the scope that set it.
TEST_F(RenderTargetCacheResourceTest, DisableCacheIsPerScope) {
  RenderTargetCache cache(allocator_);
  cache.DisableCache();
  const uint64_t scope = cache.BeginScope();
  ASSERT_TRUE(cache.CreateOffscreen(context_, {100, 100}, 1).IsValid());
  EXPECT_EQ(cache.CachedTextureCount(), 1u);
  cache.EndScope(scope);
  // Still disabled outside the scope.
  ASSERT_TRUE(cache.CreateOffscreen(context_, {50, 50}, 1).IsValid());
  EXPECT_EQ(cache.CachedTextureCount(), 1u);
  cache.EnableCache();
}

TEST_F(RenderTargetCacheResourceTest,
       ReleaseIdleAfterFrameEpochReleasesLastFrameEntries) {
  RenderTargetCache cache(allocator_);
  UseTestClock(cache);
  const uint64_t scope = cache.BeginScope();
  ASSERT_TRUE(cache.CreateOffscreen(context_, {100, 100}, 1).IsValid());
  // Leased by the open scope.
  now_ += kIdleReleaseMinUnused;
  EXPECT_EQ(cache.ReleaseIdle(kIdleReleaseMinUnused).kept_in_use, 1u);
  cache.EndScope(scope);
  cache.End();

  const auto result = cache.ReleaseIdle(kIdleReleaseMinUnused);
  EXPECT_EQ(result.before.entries, 1u);
  EXPECT_EQ(result.after.entries, 0u);
}

}  // namespace testing
}  // namespace impeller
