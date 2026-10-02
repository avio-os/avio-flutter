// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <thread>
#include "gtest/gtest.h"
#include "impeller/renderer/pipeline_resource_ledger.h"
namespace impeller {
namespace testing {
namespace {
TEST(PipelineResourceLedgerTest, NativeObjectsRemainUntilTheirGuardDies) {
  auto ledger = std::make_shared<PipelineResourceLedger>();
  PipelineResourceLedger::Registration warm(ledger, {});
  auto report = ledger->Report(true);
  EXPECT_EQ(report.entries[0].usage.entries, 1u);
  EXPECT_EQ(report.raster_thread_allocations, 0u);
  EXPECT_EQ(report.first_use_compiles, 0u);
  {
    PipelineResourceLedger::Registration frame(
        ledger, {true, AvioRasterAllocationCause::kFrameWork});
    auto moved = std::move(frame);
    report = ledger->Report(false);
    EXPECT_EQ(report.entries[0].usage.entries, 2u);
    EXPECT_EQ(report.entries[0].usage.created_entries, 1u);
    EXPECT_EQ(report.raster_thread_allocations, 1u);
    EXPECT_EQ(report.first_use_compiles, 1u);
    EXPECT_EQ(report.entries[0].usage.real_bytes, 0u);
    EXPECT_FALSE(report.entries[0].fields_supported &
                 kAvioResourceFieldActualAllocatedBytes);
  }
  EXPECT_EQ(ledger->Report(false).entries[0].usage.entries, 1u);
}
TEST(PipelineResourceLedgerTest, DeferredCompilationKeepsExactRequestOrigin) {
  auto ledger = std::make_shared<PipelineResourceLedger>();
  AvioPipelineCreationOrigin ordinary;
  AvioPipelineCreationOrigin snapshot;
  {
    AvioRasterFrameScope frame;
    ordinary = AvioPipelineCreationOrigin::Capture();
    {
      AvioRasterAllocationCauseScope cause(
          AvioRasterAllocationCause::kSnapshot);
      snapshot = AvioPipelineCreationOrigin::Capture();
    }
  }
  std::thread compiler([&] {
    EXPECT_FALSE(IsAvioRasterFrameActive());
    EXPECT_EQ(GetAvioRasterAllocationCause(),
              AvioRasterAllocationCause::kFrameWork);
    PipelineResourceLedger::Registration first(ledger, ordinary);
    PipelineResourceLedger::Registration expected(ledger, snapshot);
    const auto report = ledger->Report(false);
    EXPECT_EQ(report.first_use_compiles, 2u);
    EXPECT_EQ(report.raster_thread_allocations, 1u);
    EXPECT_EQ(report.snapshot_allocations, 1u);
    EXPECT_EQ(report.entries[0].usage.entries, 2u);
  });
  compiler.join();
  auto interval = ledger->Report(true);
  EXPECT_EQ(interval.first_use_compiles, 2u);
  EXPECT_EQ(interval.raster_thread_allocations, 1u);
  EXPECT_EQ(interval.snapshot_allocations, 1u);
  EXPECT_EQ(interval.entries[0].usage.entries, 0u);
  auto next = ledger->Report(false);
  EXPECT_EQ(next.first_use_compiles, 0u);
  EXPECT_EQ(next.raster_thread_allocations, 0u);
  EXPECT_EQ(next.snapshot_allocations, 0u);
}
TEST(PipelineResourceLedgerTest, ExpectedOperationDoesNotHideFirstUseCompile) {
  auto ledger = std::make_shared<PipelineResourceLedger>();
  PipelineResourceLedger::Registration glyph(
      ledger, {true, AvioRasterAllocationCause::kGlyphAtlasGrowth});
  PipelineResourceLedger::Registration upload(
      ledger, {true, AvioRasterAllocationCause::kImageUpload});
  PipelineResourceLedger::Registration snapshot(
      ledger, {true, AvioRasterAllocationCause::kSnapshot});
  auto report = ledger->Report(false);
  EXPECT_EQ(report.raster_thread_allocations, 0u);
  EXPECT_EQ(report.first_use_compiles, 3u);
  EXPECT_EQ(report.snapshot_allocations, 1u);
  EXPECT_EQ(report.glyph_atlas_growths, 0u);
  EXPECT_EQ(report.image_uploads, 0u);
  EXPECT_EQ(report.entries[0].usage.created_entries, 3u);
}
TEST(PipelineResourceLedgerTest, InitRequestStaysInitOnAFrameThread) {
  auto ledger = std::make_shared<PipelineResourceLedger>();
  auto cold = AvioPipelineCreationOrigin::Capture();
  {
    AvioRasterFrameScope frame;
    PipelineResourceLedger::Registration deferred_cold(ledger, cold);
    PipelineResourceLedger::Registration failed;
    EXPECT_EQ(ledger->Report(false).raster_thread_allocations, 0u);
    EXPECT_EQ(ledger->Report(false).first_use_compiles, 0u);
    EXPECT_EQ(ledger->Report(false).entries[0].usage.entries, 1u);
  }
}
}  // namespace
}  // namespace testing
}  // namespace impeller
