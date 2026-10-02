// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/avio_coverage_region.h"

#include <limits>
#include <set>
#include <thread>
#include <vector>

#include "flutter/testing/testing.h"
#include "impeller/renderer/render_resource_scope.h"

namespace impeller {
namespace testing {
namespace {

class RegionTestTexture final : public Texture {
 public:
  RegionTestTexture(TextureDescriptor desc, size_t real_bytes)
      : Texture(desc), real_bytes_(real_bytes) {}
  void SetLabel(std::string_view) override {}
  void SetLabel(std::string_view, std::string_view) override {}
  bool IsValid() const override { return true; }
  ISize GetSize() const override { return GetTextureDescriptor().size; }
  size_t GetAllocatedByteSize() const override { return real_bytes_; }

 protected:
  bool OnSetContents(const uint8_t*, size_t, size_t) override { return false; }
  bool OnSetContents(std::shared_ptr<const fml::Mapping>, size_t) override {
    return false;
  }

 private:
  size_t real_bytes_;
};

class RegionTestAllocator final : public Allocator {
 public:
  ISize GetMaxTextureSizeSupported() const override { return {4096, 4096}; }
  size_t creations = 0u;
  size_t padding = 0u;
  bool fail = false;
  bool unknown_real_bytes = false;

 protected:
  std::shared_ptr<DeviceBuffer> OnCreateBuffer(
      const DeviceBufferDescriptor&) override {
    return nullptr;
  }
  std::shared_ptr<Texture> OnCreateTexture(const TextureDescriptor& desc,
                                           bool) override {
    creations++;
    if (fail) {
      return nullptr;
    }
    size_t real = desc.GetByteSizeOfAllMipLevels() *
                  static_cast<size_t>(desc.sample_count);
    // Vulkan commonly backs D32S8 with 8 bytes, unlike nominal 5-byte texels.
    if (desc.format == PixelFormat::kD32FloatS8UInt) {
      real = desc.size.width * desc.size.height * 8u *
             static_cast<size_t>(desc.sample_count);
    }
    return std::make_shared<RegionTestTexture>(
        desc, unknown_real_bytes ? 0u : real + padding);
  }
};

// These are CPU allocation/ownership tests. The callback models a successful
// ordered full clear; backend synchronization acceptance has its own GPU gate.
std::shared_ptr<AvioCoverageRegion> CreateInitializedRegion(
    std::shared_ptr<RegionTestAllocator> allocator) {
  auto region = AvioCoverageRegion::Create(std::move(allocator), {});
  if (region) {
    EXPECT_TRUE(region->InitializeMaskOnce([] { return true; }));
    EXPECT_TRUE(region->InitializeColourOnce([] { return true; }));
  }
  return region;
}

TEST(AvioCoverageRegionTest, FullColdInitializationIsRequiredAndRunsOnce) {
  auto allocator = std::make_shared<RegionTestAllocator>();
  auto region = AvioCoverageRegion::Create(allocator, {});
  ASSERT_TRUE(region);
  ASSERT_TRUE(region->BeginRasterFrame(1u));
  EXPECT_EQ(
      region->AcquireCoverage(AvioCoverageRegion::Kind::kMask, {1, 1}).status,
      AvioCoverageRegion::Status::kNotInitialized);
  region->EndRasterFrame();
  size_t calls = 0u;
  const auto initialize = [&] {
    calls++;
    return true;
  };
  std::thread other_context(
      [&] { EXPECT_TRUE(region->InitializeMaskOnce(initialize)); });
  EXPECT_TRUE(region->InitializeMaskOnce(initialize));
  other_context.join();
  EXPECT_EQ(calls, 1u);
  ASSERT_TRUE(region->BeginRasterFrame(2u));
  EXPECT_TRUE(
      region->AcquireCoverage(AvioCoverageRegion::Kind::kMask, {1, 1}).lease);
  EXPECT_EQ(allocator->creations, 10u);
}

TEST(AvioCoverageRegionTest,
     ColdInitializationFailureNeverFallsBackOnFramePath) {
  auto allocator = std::make_shared<RegionTestAllocator>();
  auto region = AvioCoverageRegion::Create(allocator, {});
  ASSERT_TRUE(region);
  size_t calls = 0u;
  EXPECT_FALSE(region->InitializeColourOnce([&] {
    calls++;
    return false;
  }));
  EXPECT_FALSE(region->InitializeColourOnce([&] {
    calls++;
    return true;
  }));
  {
    AvioRasterFrameScope frame;
    EXPECT_FALSE(region->InitializeMaskOnce([&] {
      calls++;
      return true;
    }));
  }
  EXPECT_EQ(calls, 1u);
  EXPECT_EQ(allocator->creations, 10u);
}

TEST(AvioCoverageRegionTest, WarmRegionFactoryNeverAllocatesOnRasterFrame) {
  auto allocator = std::make_shared<RegionTestAllocator>();
  AvioRasterFrameScope frame;
  std::string error;
  EXPECT_FALSE(AvioCoverageRegion::Create(allocator, {}, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(allocator->creations, 0u);
}

TEST(AvioCoverageRegionTest, RegionsAllocatedOnceAtInit) {
  auto allocator = std::make_shared<RegionTestAllocator>();
  auto region = CreateInitializedRegion(allocator);
  ASSERT_TRUE(region);
  ASSERT_EQ(allocator->creations, 10u);
  const auto usage = region->ReportUsage(true);
  EXPECT_EQ(usage.coverage.entries, 5u);
  EXPECT_EQ(usage.coverage.real_bytes, 29u * 256u * 1024u);
  EXPECT_EQ(usage.layers.entries, 5u);
  EXPECT_LE(usage.layers.real_bytes, 4u * 1024u * 1024u);
  const auto mask = region->GetCoverageAtlasTarget().GetColorAttachment(0u);
  EXPECT_TRUE(mask.texture->GetTextureDescriptor().usage &
              TextureUsage::kShaderRead);
  EXPECT_EQ(mask.store_action, StoreAction::kStore);
  EXPECT_FALSE(mask.resolve_texture);
  for (uint64_t frame = 1u; frame < 40u; frame++) {
    ASSERT_TRUE(region->BeginRasterFrame(frame));
    auto layer = region->AcquireLayer({100, 100});
    auto mask =
        region->AcquireCoverage(AvioCoverageRegion::Kind::kMask, {60, 60});
    ASSERT_TRUE(layer.lease);
    ASSERT_TRUE(mask.lease);
    layer.lease.reset();
    mask.lease.reset();
    region->EndRasterFrame();
  }
  EXPECT_EQ(allocator->creations, 10u);
  EXPECT_EQ(region->ReportUsage(false).layers.created_entries, 0u);
}

TEST(AvioCoverageRegionTest, ActualRequirementsAreHardCappedAtInitialization) {
  auto allocator = std::make_shared<RegionTestAllocator>();
  allocator->padding = 256u * 1024u;
  std::string error;
  EXPECT_FALSE(AvioCoverageRegion::Create(allocator, {}, &error));
  EXPECT_FALSE(error.empty());
  allocator = std::make_shared<RegionTestAllocator>();
  allocator->unknown_real_bytes = true;
  EXPECT_FALSE(AvioCoverageRegion::Create(allocator, {}, &error));
}

TEST(AvioCoverageRegionTest, CoverageOverflowFlushesWithoutAllocating) {
  auto allocator = std::make_shared<RegionTestAllocator>();
  auto region = CreateInitializedRegion(allocator);
  ASSERT_TRUE(region);
  ASSERT_TRUE(region->BeginRasterFrame(1u));
  const auto size =
      region->GetCoverageTileSize(AvioCoverageRegion::Kind::kMask);
  auto full = region->AcquireCoverage(AvioCoverageRegion::Kind::kMask, size);
  ASSERT_TRUE(full.lease);
  EXPECT_EQ(
      region->AcquireCoverage(AvioCoverageRegion::Kind::kMask, {1, 1}).status,
      AvioCoverageRegion::Status::kNeedsFlush);
  bool encoded = false;
  EXPECT_FALSE(region->FlushCoverageBatch([&] {
    encoded = true;
    return true;
  }));
  EXPECT_TRUE(encoded);
  EXPECT_TRUE(full.lease->IsValid());
  ASSERT_TRUE(region->FlushCoverageBatch([&] {
    full.lease.reset();
    return true;
  }));
  EXPECT_TRUE(
      region->AcquireCoverage(AvioCoverageRegion::Kind::kMask, size).lease);
  EXPECT_EQ(allocator->creations, 10u);
  EXPECT_EQ(region->ReportUsage(false).coverage_flushes, 0u);
}

TEST(AvioCoverageRegionTest, ColourCompositeCensusPreservesLiveReaders) {
  auto allocator = std::make_shared<RegionTestAllocator>();
  auto region = CreateInitializedRegion(allocator);
  ASSERT_TRUE(region->BeginRasterFrame(1u));
  auto mask =
      region->AcquireCoverage(AvioCoverageRegion::Kind::kMask, {100, 100})
          .lease;
  auto layer = region->AcquireLayer({120, 120}).lease;
  ASSERT_TRUE(mask);
  ASSERT_TRUE(layer);
  const auto mask_content = mask->GetContentRect();
  const auto layer_texture = layer->GetRenderTarget().GetRenderTargetTexture();
  region->ReportUsage(true);
  // A failed encode and a reset vetoed by a retained clip cannot be counted as
  // successful colour-island composites.
  EXPECT_FALSE(region->FlushCoverageBatch([] { return false; }));
  EXPECT_FALSE(region->FlushCoverageBatch([] { return true; }));
  EXPECT_EQ(region->ReportUsage(false).coverage_flushes, 0u);
  region->RecordColourIslandComposite();
  region->RecordColourIslandComposite();
  EXPECT_EQ(region->ReportUsage(false).coverage_flushes, 2u);
  EXPECT_TRUE(mask->IsValid());
  EXPECT_EQ(mask->GetContentRect(), mask_content);
  EXPECT_TRUE(layer->IsValid());
  EXPECT_EQ(layer->GetRenderTarget().GetRenderTargetTexture(), layer_texture);
  EXPECT_EQ(allocator->creations, 10u);
  EXPECT_EQ(region->ReportUsage(true).coverage_flushes, 2u);
  EXPECT_EQ(region->ReportUsage(false).coverage_flushes, 0u);
  region->RecordColourIslandComposite();
  region->EndRasterFrame();
  EXPECT_EQ(region->ReportUsage(true).coverage_flushes, 1u);
  EXPECT_EQ(region->ReportUsage(false).coverage_flushes, 0u);
  EXPECT_TRUE(mask->IsValid());
  EXPECT_TRUE(layer->IsValid());
}

TEST(AvioCoverageRegionTest, CacheAndClipReadersKeepMasksAcrossFrames) {
  auto allocator = std::make_shared<RegionTestAllocator>();
  auto region = CreateInitializedRegion(allocator);
  ASSERT_TRUE(region->BeginRasterFrame(1u));
  auto cached =
      region->AcquireCoverage(AvioCoverageRegion::Kind::kMask, {100, 100})
          .lease;
  auto clip_reader = cached;
  const auto content = cached->GetContentRect();
  region->EndRasterFrame();
  EXPECT_TRUE(cached->IsValid());
  ASSERT_TRUE(region->BeginRasterFrame(2u));
  auto next =
      region->AcquireCoverage(AvioCoverageRegion::Kind::kMask, {100, 100})
          .lease;
  ASSERT_TRUE(next);
  EXPECT_NE(next->GetContentRect(), content);
  next.reset();
  EXPECT_FALSE(region->FlushCoverageBatch([&] {
    cached.reset();
    return true;
  }));
  EXPECT_TRUE(clip_reader->IsValid());
  EXPECT_TRUE(region->FlushCoverageBatch([&] {
    clip_reader.reset();
    return true;
  }));
  EXPECT_EQ(allocator->creations, 10u);
}

TEST(AvioCoverageRegionTest, NestedLayersUseIndependentPhysicalImages) {
  auto allocator = std::make_shared<RegionTestAllocator>();
  auto region = CreateInitializedRegion(allocator);
  ASSERT_TRUE(region->BeginRasterFrame(1u));
  std::vector<std::shared_ptr<AvioCoverageRegion::Lease>> leases;
  std::set<const Texture*> attachments;
  for (size_t i = 0; i < 5u; i++) {
    auto acquired = region->AcquireLayer({100, 100});
    ASSERT_TRUE(acquired.lease);
    EXPECT_FALSE(acquired.lease->IsOverflow());
    auto target = acquired.lease->GetRenderTarget();
    EXPECT_EQ(target.GetSampleCount(), SampleCount::kCount1);
    EXPECT_FALSE(target.GetDepthAttachment());
    EXPECT_FALSE(target.GetStencilAttachment());
    EXPECT_EQ(acquired.lease->GetContentRect(),
              IRect::MakeSize(ISize{100, 100}));
    EXPECT_NE(target.GetRenderTargetSize(), acquired.lease->GetRequestedSize());
    EXPECT_TRUE(
        attachments.insert(target.GetRenderTargetTexture().get()).second);
    leases.push_back(std::move(acquired.lease));
  }
  EXPECT_EQ(allocator->creations, 10u);
  auto stale = leases.back();
  stale->Release();
  auto replacement = region->AcquireLayer({100, 100});
  ASSERT_TRUE(replacement.lease);
  EXPECT_FALSE(stale->IsValid());
  EXPECT_TRUE(replacement.lease->IsValid());
  EXPECT_EQ(allocator->creations, 10u);
}

TEST(AvioCoverageRegionTest, DeferredOverflowReadPinsImageUntilLogicalRelease) {
  auto allocator = std::make_shared<RegionTestAllocator>();
  auto region = CreateInitializedRegion(allocator);
  region->ReportUsage(true);
  ASSERT_TRUE(region->BeginRasterFrame(1u));
  auto oversized = region->AcquireLayer({1000, 900});
  ASSERT_TRUE(oversized.lease);
  EXPECT_TRUE(oversized.lease->IsOverflow());
  auto submitted_reader =
      oversized.lease->GetRenderTarget().GetRenderTargetTexture();
  std::weak_ptr<Texture> weak = submitted_reader;
  auto usage = region->ReportUsage(false);
  EXPECT_EQ(usage.layer_region_overflow, 1u);
  EXPECT_EQ(usage.layer_region_overflow_real_bytes, 1000u * 900u * 4u);
  region->EndRasterFrame();
  EXPECT_TRUE(oversized.lease->IsValid());
  EXPECT_FALSE(weak.expired());
  EXPECT_EQ(region->ReportUsage(false).layers.entries, 6u);
  ASSERT_TRUE(region->BeginRasterFrame(2u));
  auto next = region->AcquireLayer({1000, 900});
  ASSERT_TRUE(next.lease);
  EXPECT_NE(submitted_reader,
            next.lease->GetRenderTarget().GetRenderTargetTexture());
  EXPECT_EQ(allocator->creations, 12u);
  oversized.lease.reset();
  EXPECT_EQ(region->ReportUsage(false).layers.entries, 6u);
  EXPECT_FALSE(weak.expired());
  submitted_reader.reset();
  EXPECT_TRUE(weak.expired());
  next.lease.reset();
  EXPECT_EQ(region->ReportUsage(false).layers.entries, 5u);
}

TEST(AvioCoverageRegionTest, CachedWarmLayerIsImmutableAcrossFrames) {
  auto allocator = std::make_shared<RegionTestAllocator>();
  auto region = CreateInitializedRegion(allocator);
  ASSERT_TRUE(region->BeginRasterFrame(1u));
  auto cached = region->AcquireLayer({120, 120}).lease;
  ASSERT_TRUE(cached);
  auto texture = cached->GetRenderTarget().GetRenderTargetTexture();
  region->EndRasterFrame();
  ASSERT_TRUE(region->BeginRasterFrame(2u));
  auto successor = region->AcquireLayer({120, 120}).lease;
  ASSERT_TRUE(successor);
  EXPECT_TRUE(cached->IsValid());
  EXPECT_NE(texture, successor->GetRenderTarget().GetRenderTargetTexture());
  cached.reset();
  auto reusable = region->AcquireLayer({120, 120}).lease;
  ASSERT_TRUE(reusable);
  EXPECT_EQ(texture, reusable->GetRenderTarget().GetRenderTargetTexture());
  EXPECT_EQ(allocator->creations, 10u);
}

TEST(AvioCoverageRegionTest,
     RuntimeEffectExactExtentNeverPretendsWarmImageSize) {
  auto allocator = std::make_shared<RegionTestAllocator>();
  auto region = CreateInitializedRegion(allocator);
  ASSERT_TRUE(region->BeginRasterFrame(1u));
  auto exact_warm = region->AcquireLayer({128, 128}, 1, true).lease;
  ASSERT_TRUE(exact_warm);
  EXPECT_FALSE(exact_warm->IsOverflow());
  auto exact_other = region->AcquireLayer({120, 120}, 1, true).lease;
  ASSERT_TRUE(exact_other);
  EXPECT_TRUE(exact_other->IsOverflow());
  EXPECT_EQ(exact_other->GetRenderTarget().GetRenderTargetSize(),
            (ISize{120, 120}));
  EXPECT_EQ(region->ReportUsage(false).layer_region_overflow, 1u);
}

TEST(AvioCoverageRegionTest, AllHiddenTrimKeepsBothRegions) {
  auto allocator = std::make_shared<RegionTestAllocator>();
  auto region = CreateInitializedRegion(allocator);
  const auto coverage =
      region->GetCoverageAtlasTarget().GetRenderTargetTexture();
  const auto colour = region->GetColourIslandTarget().GetRenderTargetTexture();
  region->TrimIdleResourceCaches();
  EXPECT_EQ(coverage,
            region->GetCoverageAtlasTarget().GetRenderTargetTexture());
  EXPECT_EQ(colour, region->GetColourIslandTarget().GetRenderTargetTexture());
  EXPECT_EQ(region->ReportUsage(false).layers.entries, 5u);
  EXPECT_EQ(allocator->creations, 10u);
}

TEST(AvioRegionPackingTest, GutterAlignmentAndRetainedReadersPreventOverwrite) {
  AvioRegionPacking packing(16, 16);
  auto first = packing.Allocate(4, 4);
  auto second = packing.Allocate(4, 4);
  ASSERT_TRUE(first.allocation);
  ASSERT_TRUE(second.allocation);
  EXPECT_EQ(first.allocation->content, (AvioRegionPacking::Rect{2, 2, 4, 4}));
  EXPECT_EQ(second.allocation->content, (AvioRegionPacking::Rect{10, 2, 4, 4}));
  EXPECT_FALSE(packing.Reset());
  EXPECT_TRUE(packing.Release(first.allocation->token));
  EXPECT_FALSE(packing.Reset());
  EXPECT_TRUE(packing.Release(second.allocation->token));
  EXPECT_TRUE(packing.Reset());
  EXPECT_FALSE(packing.IsLive(first.allocation->token));
}

TEST(AvioRegionPackingTest, StackPopDoesNotReviveOldToken) {
  AvioRegionPacking packing(16, 16, true);
  auto first = packing.Allocate(4, 4);
  auto nested = packing.Allocate(4, 4);
  ASSERT_TRUE(first.allocation);
  ASSERT_TRUE(nested.allocation);
  ASSERT_TRUE(packing.Release(nested.allocation->token));
  auto replacement = packing.Allocate(4, 4);
  ASSERT_TRUE(replacement.allocation);
  EXPECT_EQ(replacement.allocation->area, nested.allocation->area);
  EXPECT_FALSE(packing.IsLive(nested.allocation->token));
  EXPECT_FALSE(packing.Release(nested.allocation->token));
  EXPECT_TRUE(packing.IsLive(replacement.allocation->token));
}

TEST(AvioRegionPackingTest, LargePathsTileWithoutGapsOrGrowth) {
  AvioRegionTiles tiles({17, -4, 1025, 517}, 252, 252);
  size_t count = 0u;
  int64_t area = 0;
  while (const auto tile = tiles.Next()) {
    EXPECT_LE(tile->width, 252);
    EXPECT_LE(tile->height, 252);
    EXPECT_GE(tile->x, 17);
    EXPECT_GE(tile->y, -4);
    area += tile->width * tile->height;
    count++;
  }
  EXPECT_EQ(count, 15u);
  EXPECT_EQ(area, 1025 * 517);
}

TEST(AvioRegionPackingTest, ExtremeSizesDoNotOverflowOrWrapTiles) {
  const auto maximum = std::numeric_limits<int64_t>::max();
  AvioRegionPacking packing(maximum, maximum);
  EXPECT_EQ(packing.Allocate(maximum, 1).status,
            AvioRegionPacking::Status::kNeedsTiling);
  EXPECT_TRUE(packing.Allocate(maximum - 5, maximum - 5).allocation);
  AvioRegionTiles invalid({maximum, 0, 2, 2}, 1, 1);
  EXPECT_FALSE(invalid.Next());
}

}  // namespace
}  // namespace testing
}  // namespace impeller
