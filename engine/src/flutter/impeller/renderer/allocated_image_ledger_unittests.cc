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
    auto layer = ledger->Register(AvioRenderResourceKind::kLayerRegion, key, 4u,
                                  8u, IsAvioRasterFrameActive(),
                                  GetAvioRasterAllocationCause());
    auto snapshot = ledger->Register(AvioRenderResourceKind::kImageTextures,
                                     key, 4u, 8u, IsAvioRasterFrameActive(),
                                     AvioRasterAllocationCause::kSnapshot);
    auto glyph = ledger->Register(AvioRenderResourceKind::kGlyphAtlases, key,
                                  4u, 8u, IsAvioRasterFrameActive(),
                                  AvioRasterAllocationCause::kGlyphAtlasGrowth);
    std::thread io([&] {
      EXPECT_FALSE(IsAvioRasterFrameActive());
      auto image = ledger->Register(AvioRenderResourceKind::kImageTextures, key,
                                    4u, 8u, IsAvioRasterFrameActive());
    });
    io.join();
    auto report = ledger->Report(true);
    EXPECT_EQ(report.raster_thread_allocations, 1u);
    EXPECT_EQ(report.snapshot_allocations, 1u);
    EXPECT_EQ(report.glyph_atlas_growths, 1u);
    EXPECT_TRUE(report.counters_supported & kAvioCounterGlyphAtlasGrowths);
    EXPECT_TRUE(report.counters_supported & kAvioCounterImageUploads);
    EXPECT_EQ(report.image_uploads, 0u);
    auto next = ledger->Report(false);
    EXPECT_EQ(next.raster_thread_allocations, 0u);
    EXPECT_EQ(next.snapshot_allocations, 0u);
    EXPECT_EQ(next.glyph_atlas_growths, 0u);
    // Expected work is excluded from the defect counter, never from census.
    EXPECT_EQ(Usage(next, AvioRenderResourceKind::kGlyphAtlases).real_bytes,
              8u);
  }
  EXPECT_FALSE(IsAvioRasterFrameActive());
}

TEST(AllocatedImageLedgerTest, CausalExclusionsDoNotGuessFromPhysicalKind) {
  auto ledger = std::make_shared<AllocatedImageLedger>();
  AvioRasterFrameScope frame;
  auto ordinary =
      ledger->Register(AvioRenderResourceKind::kImageTextures, {}, 4u, 8u, true,
                       GetAvioRasterAllocationCause());
  {
    AvioRasterAllocationCauseScope upload(
        AvioRasterAllocationCause::kImageUpload);
    auto decoded =
        ledger->Register(AvioRenderResourceKind::kImageTextures, {}, 4u, 8u,
                         true, GetAvioRasterAllocationCause());
    auto report = ledger->Report(false);
    EXPECT_EQ(report.raster_thread_allocations, 1u);
    EXPECT_EQ(Usage(report, AvioRenderResourceKind::kImageTextures).entries,
              2u);
    EXPECT_EQ(Usage(report, AvioRenderResourceKind::kImageTextures).real_bytes,
              16u);
    EXPECT_EQ(report.image_uploads, 0u);  // not an encoded upload yet
    decoded.RecordImageUpload(true);
    EXPECT_EQ(ledger->Report(false).image_uploads, 1u);
  }
  EXPECT_EQ(GetAvioRasterAllocationCause(),
            AvioRasterAllocationCause::kFrameWork);
  // A buffer/scratch created by the glyph operation is excluded too, but
  // cannot falsely certify that a native atlas grew.
  auto scratch =
      ledger->Register(AvioRenderResourceKind::kLayerRegion, {}, 4u, 8u, true,
                       AvioRasterAllocationCause::kGlyphAtlasGrowth);
  EXPECT_EQ(ledger->Report(false).glyph_atlas_growths, 0u);
}

TEST(AllocatedImageLedgerTest, NestedOperationsPreserveSnapshotCauseAndGrowth) {
  auto ledger = std::make_shared<AllocatedImageLedger>();
  AvioRasterFrameScope frame;
  {
    AvioRasterAllocationCauseScope snapshot(
        AvioRasterAllocationCause::kSnapshot);
    {
      AvioRasterAllocationCauseScope glyph(
          AvioRasterAllocationCause::kGlyphAtlasGrowth);
      EXPECT_EQ(GetAvioRasterAllocationCause(),
                AvioRasterAllocationCause::kSnapshot);
      auto atlas =
          ledger->Register(AvioRenderResourceKind::kGlyphAtlases, {}, 4u, 8u,
                           true, GetAvioRasterAllocationCause());
      const auto report = ledger->Report(false);
      EXPECT_EQ(report.raster_thread_allocations, 0u);
      EXPECT_EQ(report.snapshot_allocations, 1u);
      EXPECT_EQ(report.glyph_atlas_growths, 1u);
      {
        AvioRasterAllocationCauseScope nested(
            AvioRasterAllocationCause::kFrameWork);
        EXPECT_EQ(GetAvioRasterAllocationCause(),
                  AvioRasterAllocationCause::kSnapshot);
      }
    }
    EXPECT_EQ(GetAvioRasterAllocationCause(),
              AvioRasterAllocationCause::kSnapshot);
  }
  EXPECT_EQ(GetAvioRasterAllocationCause(),
            AvioRasterAllocationCause::kFrameWork);
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

TEST(AllocatedImageLedgerTest,
     PhysicalDescriptorMultiplicityIsAvailableWithoutLeaseInference) {
  auto ledger = std::make_shared<AllocatedImageLedger>();
  auto image = ledger->Register(AvioRenderResourceKind::kImageTextures, {}, 4u,
                                8u, false);
  const auto report = ledger->Report(false);
  ASSERT_EQ(report.entries_count, 1u);
  const auto& entry = report.entries[0];
  EXPECT_EQ(entry.fields_supported,
            kAvioResourceFieldCounts | kAvioResourceFieldDescriptorBytes |
                kAvioResourceFieldActualAllocatedBytes |
                kAvioResourceFieldDescriptorMultiplicity);
  EXPECT_FALSE(entry.fields_supported & kAvioResourceFieldLeases);
  EXPECT_FALSE(entry.fields_supported & kAvioResourceFieldTextureDescriptor);
  EXPECT_EQ(entry.usage.entries, 1u);
  EXPECT_EQ(entry.usage.created_entries, 1u);
  EXPECT_EQ(entry.usage.distinct_keys, 1u);
  EXPECT_EQ(entry.usage.real_bytes, 8u);
}

}  // namespace
}  // namespace testing
}  // namespace impeller
