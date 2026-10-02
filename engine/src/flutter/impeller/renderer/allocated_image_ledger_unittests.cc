// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/allocated_image_ledger.h"

#include <thread>
#include <utility>

#include "gtest/gtest.h"
#include "impeller/renderer/render_resource_scope.h"

namespace impeller {
namespace testing {
namespace {

RenderResourceUsage Usage(const AvioRenderResourceReport& report,
                          AvioRenderResourceKind kind) {
  for (size_t i = 0u; i < report.entries_count; i++) {
    if (report.entries[i].kind_id == static_cast<uint32_t>(kind)) {
      return report.entries[i].usage;
    }
  }
  return {};
}

TEST(AllocatedImageLedgerTest, TypedBytesStayUntilActualAllocationGuardDies) {
  auto ledger = std::make_shared<AllocatedImageLedger>();
  AvioAllocatedImageKey key;
  key.fields[0] = 1024u;
  auto resource = ledger->Register(AvioRenderResourceKind::kCoverageRegion, key,
                                   1024u, 4096u, false);
  auto moved = std::move(resource);
  auto usage =
      Usage(ledger->Report(false), AvioRenderResourceKind::kCoverageRegion);
  EXPECT_EQ(usage.entries, 1u);
  EXPECT_EQ(usage.nominal_bytes, 1024u);
  EXPECT_EQ(usage.real_bytes, 4096u);
  moved = {};
  usage = Usage(ledger->Report(false), AvioRenderResourceKind::kCoverageRegion);
  EXPECT_EQ(usage.entries, 0u);
  EXPECT_EQ(usage.real_bytes, 0u);
  EXPECT_EQ(usage.orphans_released_real_bytes, 4096u);
}

TEST(AllocatedImageLedgerTest, CompleteIdentityAndMovingIntrusiveRecords) {
  auto ledger = std::make_shared<AllocatedImageLedger>();
  AvioAllocatedImageKey key;
  auto first = ledger->Register(AvioRenderResourceKind::kLayerRegion, key, 100u,
                                128u, false);
  auto second = ledger->Register(AvioRenderResourceKind::kLayerRegion, key,
                                 100u, 128u, false);
  key.fields[11] = 1u;
  auto third = ledger->Register(AvioRenderResourceKind::kLayerRegion, key, 100u,
                                128u, false);
  auto usage =
      Usage(ledger->Report(false), AvioRenderResourceKind::kLayerRegion);
  EXPECT_EQ(usage.distinct_keys, 2u);
  EXPECT_EQ(usage.duplicate_entries, 1u);
  first = std::move(third);
  usage = Usage(ledger->Report(false), AvioRenderResourceKind::kLayerRegion);
  EXPECT_EQ(usage.entries, 2u);
  EXPECT_EQ(usage.distinct_keys, 2u);
  EXPECT_EQ(usage.duplicate_entries, 0u);
  second = {};
  auto* same_registration = &first;
  first = std::move(*same_registration);
  EXPECT_EQ(Usage(ledger->Report(false), AvioRenderResourceKind::kLayerRegion)
                .real_bytes,
            128u);
}

TEST(AllocatedImageLedgerTest,
     ExplicitRasterOriginExcludesInitializationAndIo) {
  auto ledger = std::make_shared<AllocatedImageLedger>();
  AvioAllocatedImageKey key;
  auto warm = ledger->Register(AvioRenderResourceKind::kCoverageRegion, key, 4u,
                               8u, IsAvioRasterFrameActive());
  EXPECT_EQ(ledger->Report(true).raster_thread_allocations, 0u);
  {
    AvioRasterFrameScope frame;
    auto snapshot = ledger->Register(AvioRenderResourceKind::kLayerRegion, key,
                                     4u, 8u, IsAvioRasterFrameActive());
    auto glyph = ledger->Register(AvioRenderResourceKind::kGlyphAtlases, key,
                                  4u, 8u, IsAvioRasterFrameActive());
    std::thread io([&] {
      EXPECT_FALSE(IsAvioRasterFrameActive());
      auto image = ledger->Register(AvioRenderResourceKind::kImageTextures, key,
                                    4u, 8u, IsAvioRasterFrameActive());
    });
    io.join();
    auto report = ledger->Report(true);
    EXPECT_EQ(report.raster_thread_allocations, 2u);
    EXPECT_EQ(report.snapshot_allocations, 1u);
    EXPECT_FALSE(report.counters_supported & kAvioCounterGlyphAtlasGrowths);
    EXPECT_TRUE(report.counters_supported & kAvioCounterImageUploads);
    EXPECT_EQ(report.image_uploads, 0u);
    EXPECT_EQ(ledger->Report(false).raster_thread_allocations, 0u);
  }
  EXPECT_FALSE(IsAvioRasterFrameActive());
}

TEST(AllocatedImageLedgerTest, NestedScopesRestoreAndNeverGuessFromLabels) {
  EXPECT_EQ(GetAvioResourceAllocationKind(),
            AvioRenderResourceKind::kImageTextures);
  {
    AvioResourceAllocationScope flip(AvioRenderResourceKind::kFlipTargets);
    EXPECT_EQ(GetAvioResourceAllocationKind(),
              AvioRenderResourceKind::kFlipTargets);
    {
      AvioResourceAllocationScope transient(
          AvioRenderResourceKind::kTransientAttachments);
      EXPECT_EQ(GetAvioResourceAllocationKind(),
                AvioRenderResourceKind::kTransientAttachments);
      AvioRasterFrameScope outer;
      {
        AvioRasterFrameScope inner;
        EXPECT_TRUE(IsAvioRasterFrameActive());
      }
      EXPECT_TRUE(IsAvioRasterFrameActive());
    }
    EXPECT_FALSE(IsAvioRasterFrameActive());
    EXPECT_EQ(GetAvioResourceAllocationKind(),
              AvioRenderResourceKind::kFlipTargets);
  }
  EXPECT_EQ(GetAvioResourceAllocationKind(),
            AvioRenderResourceKind::kImageTextures);
}

TEST(AllocatedImageLedgerTest,
     ImageUploadsRequireClassifiedOwnerAndRasterOrigin) {
  auto ledger = std::make_shared<AllocatedImageLedger>();
  auto image = ledger->Register(AvioRenderResourceKind::kImageTextures, {}, 4u,
                                8u, false);
  auto glyph = ledger->Register(AvioRenderResourceKind::kGlyphAtlases, {}, 4u,
                                8u, false);
  auto scratch =
      ledger->Register(AvioRenderResourceKind::kLayerRegion, {}, 4u, 8u, false);
  image.RecordImageUpload(true);
  glyph.RecordImageUpload(true);
  image.RecordImageUpload(false);
  scratch.RecordImageUpload(true);
  AllocatedImageLedger::Registration imported;
  imported.RecordImageUpload(true);
  EXPECT_EQ(ledger->Report(true).image_uploads, 2u);
  EXPECT_EQ(ledger->Report(false).image_uploads, 0u);
}

TEST(AllocatedImageLedgerTest, ConcurrentIntervalsDoNotLoseActualCreations) {
  auto ledger = std::make_shared<AllocatedImageLedger>();
  std::thread creator([&] {
    for (size_t i = 0; i < 500u; i++) {
      auto guard = ledger->Register(AvioRenderResourceKind::kImageTextures, {},
                                    4u, 8u, true);
    }
  });
  uint64_t total = 0u;
  for (size_t i = 0; i < 500u; i++) {
    total += ledger->Report(true).raster_thread_allocations;
  }
  creator.join();
  total += ledger->Report(true).raster_thread_allocations;
  EXPECT_EQ(total, 500u);
  EXPECT_EQ(Usage(ledger->Report(false), AvioRenderResourceKind::kImageTextures)
                .entries,
            0u);
}

}  // namespace
}  // namespace testing
}  // namespace impeller
