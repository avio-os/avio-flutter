// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/allocated_buffer_ledger.h"

#include <thread>

#include "gtest/gtest.h"

namespace impeller {
namespace {
TEST(AllocatedBufferLedgerTest, PhysicalBytesAndDeferredOwnerLifetime) {
  auto ledger = std::make_shared<AllocatedBufferLedger>();
  auto owner = std::make_shared<AllocatedBufferLedger::Registration>(
      ledger->Register(101, 256, false));
  auto submitted_owner = owner;
  owner.reset();
  const auto live = ledger->Report(false);
  ASSERT_EQ(live.entries_count, 1u);
  EXPECT_EQ(live.entries[0].kind_id, 9u);
  EXPECT_EQ(live.entries[0].usage.entries, 1u);
  EXPECT_EQ(live.entries[0].usage.nominal_bytes, 101u);
  EXPECT_EQ(live.entries[0].usage.real_bytes, 256u);
  EXPECT_EQ(live.entries[0].fields_supported, 7u);
  EXPECT_EQ(live.entries[0].usage.created_entries, 1u);
  submitted_owner.reset();
  const auto dead = ledger->Report(false);
  EXPECT_EQ(dead.entries[0].usage.entries, 0u);
  EXPECT_EQ(dead.entries[0].usage.real_bytes, 0u);
  EXPECT_EQ(dead.entries[0].usage.orphans_released_entries, 1u);
  EXPECT_EQ(dead.entries[0].usage.orphans_released_real_bytes, 256u);
}

TEST(AllocatedBufferLedgerTest, MoveAssignmentDropsOnlyTheReplacedResource) {
  auto ledger = std::make_shared<AllocatedBufferLedger>();
  auto first = ledger->Register(100, 128, true);
  auto second = ledger->Register(200, 256, true);
  first = std::move(second);
  auto moved = std::move(first);
  auto report = ledger->Report(false);
  EXPECT_EQ(report.entries[0].usage.entries, 1u);
  EXPECT_EQ(report.entries[0].usage.nominal_bytes, 200u);
  EXPECT_EQ(report.entries[0].usage.orphans_released_entries, 1u);
  EXPECT_EQ(report.raster_thread_allocations, 2u);
  moved = {};
  EXPECT_EQ(ledger->Report(false).entries[0].usage.entries, 0u);
}

TEST(AllocatedBufferLedgerTest, IntervalResetDoesNotLoseLivePhysicalBytes) {
  auto ledger = std::make_shared<AllocatedBufferLedger>();
  auto warm = ledger->Register(42, 64, false);
  auto raster = ledger->Register(130, 256, true);
  auto first = ledger->Report(true);
  EXPECT_EQ(first.raster_thread_allocations, 1u);
  EXPECT_EQ(first.entries[0].usage.created_entries, 2u);
  auto second = ledger->Report(false);
  EXPECT_EQ(second.raster_thread_allocations, 0u);
  EXPECT_EQ(second.entries[0].usage.created_entries, 0u);
  EXPECT_EQ(second.entries[0].usage.real_bytes, 320u);
  warm = {};
  auto third = ledger->Report(true);
  EXPECT_EQ(third.entries[0].usage.orphans_released_real_bytes, 64u);
  EXPECT_EQ(ledger->Report(false).entries[0].usage.orphans_released_entries,
            0u);
}

TEST(AllocatedBufferLedgerTest, NativeDestructorPrecedesCensusDecrement) {
  auto ledger = std::make_shared<AllocatedBufferLedger>();
  struct NativeOwner {
    std::shared_ptr<AllocatedBufferLedger> ledger;
    bool* saw_live;
    ~NativeOwner() {
      *saw_live = ledger->Report(false).entries[0].usage.entries == 1;
    }
  };
  struct Resource {
    AllocatedBufferLedger::Registration registration;
    NativeOwner native;
  };
  bool saw_live = false;
  {
    Resource resource{ledger->Register(8, 16, true), {ledger, &saw_live}};
  }
  EXPECT_TRUE(saw_live);
  EXPECT_EQ(ledger->Report(false).entries[0].usage.entries, 0u);
}

TEST(AllocatedBufferLedgerTest, CrossThreadReleaseAndProviderMerge) {
  auto ledger = std::make_shared<AllocatedBufferLedger>();
  auto guard = ledger->Register(1000, 1024, true);
  AvioRenderResourceReport images;
  images.counters_supported = kAvioCounterRasterThreadAllocations;
  images.raster_thread_allocations = 3u;
  images.Merge(ledger->Report(false));
  EXPECT_EQ(images.raster_thread_allocations, 4u);
  std::thread worker([guard = std::move(guard)]() mutable { guard = {}; });
  worker.join();
  EXPECT_EQ(ledger->Report(false).entries[0].usage.entries, 0u);
  EXPECT_EQ(ledger->Report(false).entries[0].usage.orphans_released_entries,
            1u);
}
TEST(AllocatedBufferLedgerTest, ExplicitCauseExclusionsDoNotHidePhysicalBytes) {
  auto ledger = std::make_shared<AllocatedBufferLedger>();
  auto work = ledger->Register(100, 128, true);
  auto glyph = ledger->Register(200, 256, true,
                                AvioRasterAllocationCause::kGlyphAtlasGrowth);
  auto upload =
      ledger->Register(300, 512, true, AvioRasterAllocationCause::kImageUpload);
  auto snapshot =
      ledger->Register(400, 512, true, AvioRasterAllocationCause::kSnapshot);
  const auto report = ledger->Report(false);
  EXPECT_EQ(report.raster_thread_allocations, 1u);
  EXPECT_EQ(report.snapshot_allocations, 1u);
  EXPECT_EQ(report.entries[0].usage.entries, 4u);
  EXPECT_EQ(report.entries[0].usage.nominal_bytes, 1000u);
  EXPECT_EQ(report.entries[0].usage.real_bytes, 1408u);
  EXPECT_EQ(report.entries[0].usage.created_entries, 4u);
}

}  // namespace
}  // namespace impeller
