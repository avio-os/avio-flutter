// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Pure production contract target: no GN-generated shaders or GPU required.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>

#include "flutter/shell/platform/embedder/avio_antialiasing_config.h"
#include "flutter/shell/platform/embedder/avio_render_resource_report.h"
#include "impeller/renderer/pipeline_resource_ledger.h"
#include "impeller/renderer/render_resource_scope.h"

thread_local bool track_heap = false;
thread_local size_t heap_allocations = 0;
void* operator new(size_t size) {
  heap_allocations += track_heap;
  if (void* memory = std::malloc(size ? size : 1)) {
    return memory;
  }
  std::abort();
}
void operator delete(void* memory) noexcept {
  std::free(memory);
}
void operator delete(void* memory, size_t) noexcept {
  std::free(memory);
}

namespace {
constexpr auto kAA = kFlutterAvioExtensionFeatureAntialiasingPolicy;
FlutterAvioAntialiasingConfig Coverage() {
  return {sizeof(FlutterAvioAntialiasingConfig),
          kFlutterAvioAntialiasingPolicyCoverage,
          1,
          4,
          0,
          8u * 1024u * 1024u,
          4u * 1024u * 1024u};
}

void AntialiasingValidationFailsClosed() {
  const impeller::AvioAntialiasingConfig defaults;
  assert(!defaults.UsesCoverage() && defaults.layer_sample_count == 4 &&
         defaults.coverage_sample_count == 4 &&
         defaults.coverage_region_max_bytes == 0 &&
         defaults.layer_region_max_bytes == 0);
  auto config = Coverage();
  assert(!flutter::ValidateAvioAntialiasingConfig(nullptr, 0));
  assert(flutter::ValidateAvioAntialiasingConfig(&config, 0));
  assert(flutter::ValidateAvioAntialiasingConfig(nullptr, kAA));
  assert(flutter::ValidateAvioAntialiasingConfig(&config, kAA, false));
  assert(!flutter::ValidateAvioAntialiasingConfig(&config, kAA, true));
  assert((flutter::ValidateAvioAntialiasingConfig(&config, kAA) != nullptr) ==
         !impeller::kAvioCoveragePolicyImplemented);
  const auto internal = flutter::CopyAvioAntialiasingConfig(config);
  assert(internal.UsesCoverage() && internal.layer_sample_count == 1 &&
         internal.coverage_sample_count == 4 &&
         internal.coverage_region_max_bytes == 8u * 1024u * 1024u &&
         internal.layer_region_max_bytes == 4u * 1024u * 1024u);
  config.struct_size =
      offsetof(FlutterAvioAntialiasingConfig, layer_region_max_bytes);
  assert(flutter::ValidateAvioAntialiasingConfig(&config, kAA, true));
  config = Coverage();
  config.layer_sample_count = 4;
  assert(flutter::ValidateAvioAntialiasingConfig(&config, kAA, true));
  config = Coverage();
  config.coverage_sample_count = 1;
  assert(flutter::ValidateAvioAntialiasingConfig(&config, kAA, true));
  config = Coverage();
  config.continuous_requested_classes = uint64_t{1} << 63;
  assert(flutter::ValidateAvioAntialiasingConfig(&config, kAA, true));
  config = Coverage();
  config.coverage_region_max_bytes = 0;
  assert(flutter::ValidateAvioAntialiasingConfig(&config, kAA, true));
  config = Coverage();
  config.policy = static_cast<FlutterAvioAntialiasingPolicy>(91);
  assert(flutter::ValidateAvioAntialiasingConfig(&config, kAA, true));
  config = {sizeof(config), kFlutterAvioAntialiasingPolicyMsaa4, 4, 4, 0, 0, 0};
  assert(!flutter::ValidateAvioAntialiasingConfig(&config, kAA, false));
  config.layer_region_max_bytes = 1;
  assert(flutter::ValidateAvioAntialiasingConfig(&config, kAA, false));
}

struct Observation {
  unsigned callbacks = 0;
  FlutterAvioRenderResourceReportStatus status =
      kFlutterAvioRenderResourceReportEngineUnavailable;
  size_t count = 0;
  uint32_t kind = 0;
  uint64_t real_bytes = 0;
  uint64_t counters = 0;
  uint64_t allocations = 0;
  std::thread::id thread;
};
void Observe(const FlutterAvioRenderResourceReport* report, void* opaque) {
  auto& result = *static_cast<Observation*>(opaque);
  result.callbacks++;
  result.status = report->status;
  result.count = report->entries_count;
  result.counters = report->counters_supported;
  result.allocations = report->raster_thread_allocations;
  result.thread = std::this_thread::get_id();
  if (report->entries_count) {
    assert(report->entries);
    assert(report->entries_count <= FLUTTER_AVIO_MAX_RENDER_RESOURCE_ENTRIES);
    result.kind = report->entries[0].kind_id;
    result.real_bytes = report->entries[0].real_bytes;
    assert(report->entries[0].struct_size == sizeof(*report->entries));
  } else {
    assert(!report->entries);
  }
}

void UnknownKindsAndExcludedCountersSurviveWithoutAllocating() {
  impeller::AvioRenderResourceReport source;
  source.available = true;
  source.AddEntry(0xFFFFFFFFu, {.entries = 2, .real_bytes = 123456789});
  source.AddCoverageReason({0xABCDEF01u, 12, 9000});
  source.counters_supported = impeller::kAvioCounterRasterThreadAllocations |
                              impeller::kAvioCounterGlyphAtlasGrowths;
  source.raster_thread_allocations = 7;
  source.glyph_atlas_growths = 11;
  Observation result;
  const size_t before = heap_allocations;
  track_heap = true;
  flutter::DeliverAvioRenderResourceReport(
      Observe, &result, source, kFlutterAvioRenderResourceReportSuccess);
  flutter::DeliverAvioRenderResourceReport(
      [](const FlutterAvioRenderResourceReport* report, void*) {
        assert(report->glyph_atlas_growths == 11);
        assert(report->coverage_reasons_count == 1);
        assert(report->coverage_reasons[0].reason_id == 0xABCDEF01u);
        assert(report->coverage_reasons[0].draw_count == 12);
        assert(report->coverage_reasons[0].pixel_area == 9000);
      },
      nullptr, source, kFlutterAvioRenderResourceReportSuccess);
  track_heap = false;
  assert(heap_allocations == before);
  assert(result.callbacks == 1 && result.count == 1);
  assert(result.kind == 0xFFFFFFFFu && result.real_bytes == 123456789);
  assert(result.counters == 5 && result.allocations == 7);
  assert(source.entries_count == 1 &&
         source.entries[0].usage.real_bytes == 123456789);
  assert(source.glyph_atlas_growths == 11);
}

void ReportsStayBoundedAndTagOverflow() {
  impeller::AvioRenderResourceReport source;
  for (uint32_t i = 0; i < source.kMaxEntries; i++) {
    assert(source.AddEntry(i + 1, {}));
  }
  assert(!source.AddEntry(99, {}));
  Observation result;
  flutter::DeliverAvioRenderResourceReport(
      Observe, &result, source, kFlutterAvioRenderResourceReportSuccess);
  assert(result.status == kFlutterAvioRenderResourceReportTruncated);
  assert(result.count == FLUTTER_AVIO_MAX_RENDER_RESOURCE_ENTRIES);
  source.entries_count += 100;
  flutter::DeliverAvioRenderResourceReport(
      Observe, &result, source, kFlutterAvioRenderResourceReportSuccess);
  assert(result.count == FLUTTER_AVIO_MAX_RENDER_RESOURCE_ENTRIES);
}

void MergeIndependentProvidersPreservesAvailabilityAndReasons() {
  impeller::AvioRenderResourceReport context;
  context.available = true;
  context.AddEntry(impeller::AvioRenderResourceKind::kTransientAttachments,
                   {.real_bytes = 100});
  context.counters_supported = impeller::kAvioCounterRasterThreadAllocations;
  context.raster_thread_allocations = 5;
  impeller::AvioRenderResourceReport renderer;
  renderer.AddEntry(0xABCDEFFFu, {.real_bytes = 200});
  renderer.AddCoverageReason({17, 3, 42});
  renderer.counters_supported = impeller::kAvioCounterGlyphAtlasGrowths;
  renderer.glyph_atlas_growths = 2;
  // Unimplemented fields must not leak into a supported provider's counter.
  renderer.raster_thread_allocations = 999;
  context.Merge(renderer);
  assert(context.available && !context.truncated);
  assert(context.entries_count == 2 &&
         context.entries[1].kind_id == 0xABCDEFFFu);
  assert(context.raster_thread_allocations == 5 &&
         context.glyph_atlas_growths == 2);
  assert(context.counters_supported == 5 &&
         context.coverage_reasons_count == 1);
  assert(context.coverage_reasons[0].pixel_area == 42);
  assert(renderer.entries_count == 1 &&
         renderer.raster_thread_allocations == 999);
  renderer.coverage_reasons_count = renderer.kMaxCoverageReasons + 1;
  context.Merge(renderer);
  assert(context.truncated);
  assert(context.coverage_reasons_count == context.kMaxCoverageReasons);
}

void EventCountersPreserveUnitsAndAvailability() {
  impeller::AvioRenderResourceReport total;
  impeller::AvioRenderResourceReport absent;
  absent.coverage_flushes = 999;
  absent.layer_region_overflows = 999;
  absent.layer_region_overflow_real_bytes = 999;
  total.Merge(absent);
  assert(total.counters_supported == 0 && total.coverage_flushes == 0 &&
         total.layer_region_overflows == 0 &&
         total.layer_region_overflow_real_bytes == 0);
  impeller::AvioRenderResourceReport region;
  region.available = true;
  region.counters_supported = impeller::kAvioCounterCoverageFlushes |
                              impeller::kAvioCounterLayerRegionOverflows;
  region.coverage_flushes = 3;
  region.layer_region_overflows = 2;
  region.layer_region_overflow_real_bytes = 4096;
  heap_allocations = 0;
  track_heap = true;
  total.Merge(region);
  total.Merge(region);
  flutter::DeliverAvioRenderResourceReport(
      [](const FlutterAvioRenderResourceReport* report, void*) {
        assert(report->struct_size == sizeof(*report));
        assert(report->counters_supported ==
               (kFlutterAvioRenderCounterCoverageFlushes |
                kFlutterAvioRenderCounterLayerRegionOverflows));
        assert(report->coverage_flushes == 6 &&
               report->layer_region_overflows == 4 &&
               report->layer_region_overflow_real_bytes == 8192);
        assert(report->coverage_reasons_count == 0);
      },
      nullptr, total, kFlutterAvioRenderResourceReportSuccess);
  track_heap = false;
  assert(heap_allocations == 0);
  assert(region.coverage_flushes == 3 && region.layer_region_overflows == 2);
}

void RunnerCompletionOnceAndShutdownCancellation() {
  flutter::AvioRenderResourceReportRequests requests;
  Observation completed;
  Observation cancelled;
  const auto ticket = requests.Reserve(Observe, &completed);
  const auto pending = requests.Reserve(Observe, &cancelled);
  assert(ticket && pending && ticket != pending);
  assert(completed.callbacks == 0);
  std::thread runner([&] {
    impeller::AvioRenderResourceReport report;
    report.available = true;
    requests.Complete(ticket, report, kFlutterAvioRenderResourceReportSuccess);
    requests.Complete(ticket, report, kFlutterAvioRenderResourceReportSuccess);
    requests.Close();
    requests.Complete(pending, report, kFlutterAvioRenderResourceReportSuccess);
    requests.Close();
  });
  const auto runner_id = runner.get_id();
  runner.join();
  assert(completed.callbacks == 1 && completed.thread == runner_id);
  assert(completed.status == kFlutterAvioRenderResourceReportSuccess);
  assert(cancelled.callbacks == 1 && cancelled.thread == runner_id);
  assert(cancelled.status == kFlutterAvioRenderResourceReportEngineUnavailable);
  assert(requests.Reserve(Observe, &completed) == 0);
}

void RejectionNoCallbackAndCallbackOutsideLocks() {
  flutter::AvioRenderResourceReportRequests requests;
  Observation observation;
  assert(requests.Reserve(nullptr, &observation) == 0);
  std::array<uint64_t, FLUTTER_AVIO_MAX_RENDER_RESOURCE_ENTRIES> tickets;
  for (auto& ticket : tickets) {
    ticket = requests.Reserve(Observe, &observation);
    assert(ticket);
  }
  assert(requests.Reserve(Observe, &observation) == 0);
  assert(observation.callbacks == 0);
  requests.Close();
  assert(observation.callbacks == tickets.size());
  flutter::AvioRenderResourceReportRequests reentrant;
  const auto ticket = reentrant.Reserve(
      [](const FlutterAvioRenderResourceReport*, void* ptr) {
        static_cast<flutter::AvioRenderResourceReportRequests*>(ptr)->Close();
      },
      &reentrant);
  reentrant.Complete(ticket, {}, kFlutterAvioRenderResourceReportSuccess);
}

void PipelineLifetimeAndIntervalDoNotAllocate() {
  auto ledger = std::make_shared<impeller::PipelineResourceLedger>();
  using Registration = impeller::PipelineResourceLedger::Registration;
  heap_allocations = 0;
  track_heap = true;
  {
    Registration warm(ledger, false);
    Registration first(ledger, true);
    Registration variant(ledger, true);
    Registration moved(std::move(variant));
    auto report = ledger->Report(true);
    assert(report.entries_count == 1 && report.entries[0].usage.entries == 3 &&
           report.entries[0].usage.created_entries == 3 &&
           report.entries[0].usage.real_bytes == 0 &&
           report.counters_supported ==
               impeller::kAvioCounterFirstUseCompiles &&
           report.first_use_compiles == 2);
    report = ledger->Report(false);
    assert(report.entries[0].usage.entries == 3 &&
           report.entries[0].usage.created_entries == 0 &&
           report.first_use_compiles == 0);
    moved = Registration();
    assert(ledger->Report(false).entries[0].usage.entries == 2);
  }
  assert(ledger->Report(false).entries[0].usage.entries == 0);
  track_heap = false;
  assert(heap_allocations == 0);
  const std::weak_ptr<impeller::PipelineResourceLedger> lifetime = ledger;
  {
    Registration submitted(ledger, false);
    ledger.reset();
    assert(!lifetime.expired());
  }
  assert(lifetime.expired());
}

void PipelineFirstUseOriginSurvivesAsyncWorker() {
  auto ledger = std::make_shared<impeller::PipelineResourceLedger>();
  assert(!impeller::IsAvioRasterFrameActive());
  bool captured_origin = false;
  heap_allocations = 0;
  track_heap = true;
  {
    const impeller::AvioRasterFrameScope outer;
    {
      const impeller::AvioRasterFrameScope inner;
      captured_origin = impeller::IsAvioRasterFrameActive();
    }
    assert(impeller::IsAvioRasterFrameActive());
  }
  assert(!impeller::IsAvioRasterFrameActive());
  track_heap = false;
  assert(heap_allocations == 0);
  std::thread worker([ledger, captured_origin] {
    assert(!impeller::IsAvioRasterFrameActive());
    impeller::PipelineResourceLedger::Registration created(ledger,
                                                           captured_origin);
    const auto report = ledger->Report(false);
    assert(report.first_use_compiles == 1 &&
           report.entries[0].usage.entries == 1);
  });
  worker.join();
  const auto report = ledger->Report(true);
  assert(report.first_use_compiles == 1 &&
         report.entries[0].usage.created_entries == 1 &&
         report.entries[0].usage.entries == 0);
}

void ConcurrentPipelineIntervalsLoseNoSuccessfulCreations() {
  auto ledger = std::make_shared<impeller::PipelineResourceLedger>();
  constexpr size_t kCreations = 2048;
  std::atomic_bool done = false;
  std::thread worker([&] {
    for (size_t i = 0; i < kCreations; i++) {
      impeller::PipelineResourceLedger::Registration created(ledger, i % 2);
    }
    done.store(true);
  });
  size_t creations = 0;
  uint64_t first_use = 0;
  while (!done.load()) {
    const auto report = ledger->Report(true);
    creations += report.entries[0].usage.created_entries;
    first_use += report.first_use_compiles;
  }
  worker.join();
  const auto remaining = ledger->Report(true);
  creations += remaining.entries[0].usage.created_entries;
  first_use += remaining.first_use_compiles;
  assert(creations == kCreations && first_use == kCreations / 2 &&
         remaining.entries[0].usage.entries == 0);
}
}  // namespace

int main() {
  static_assert(FLUTTER_AVIO_EXTENSION_VERSION == 8);
  static_assert(impeller::kAvioContinuousSupportedClasses == 0);
  static_assert(offsetof(FlutterProjectArgs, avio_antialiasing_config) >
                offsetof(FlutterProjectArgs, avio_resource_lifecycle_config));
  static_assert(
      offsetof(FlutterEngineProcTable, RequestAvioRenderResourceReport) >
      offsetof(FlutterEngineProcTable, SetAvioViewVisibility));
  AntialiasingValidationFailsClosed();
  UnknownKindsAndExcludedCountersSurviveWithoutAllocating();
  ReportsStayBoundedAndTagOverflow();
  MergeIndependentProvidersPreservesAvailabilityAndReasons();
  EventCountersPreserveUnitsAndAvailability();
  RunnerCompletionOnceAndShutdownCancellation();
  RejectionNoCallbackAndCallbackOutsideLocks();
  PipelineLifetimeAndIntervalDoNotAllocate();
  PipelineFirstUseOriginSurvivesAsyncWorker();
  ConcurrentPipelineIntervalsLoseNoSuccessfulCreations();
  std::puts(
      "Avio coverage ABI/report production contracts: 10 passed, 0 skipped");
  std::printf(
      "Production readiness: coverage=%s, continuous_classes=0\n",
      impeller::kAvioCoveragePolicyImplemented ? "enabled" : "disabled");
}
