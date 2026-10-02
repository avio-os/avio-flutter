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
#include <limits>
#include <new>
#include <thread>

#include "flutter/shell/platform/embedder/avio_antialiasing_config.h"
#include "flutter/shell/platform/embedder/avio_frame_facts.h"
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

void ContinuousClassAdmissionIsBackendSpecific() {
  auto config = Coverage();
  assert(config.continuous_requested_classes == 0);
  for (auto backend : {impeller::AvioCoverageBackend::kVulkan,
                       impeller::AvioCoverageBackend::kGLES,
                       impeller::AvioCoverageBackend::kMetal}) {
    assert(!flutter::ValidateAvioAntialiasingBackend(config, backend));
    for (uint64_t shape = 1; shape <= (1u << 6); shape <<= 1) {
      config.continuous_requested_classes = shape;
      assert(!flutter::ValidateAvioAntialiasingConfig(&config, kAA, true));
      const bool implemented =
          (impeller::AvioContinuousSupportedClasses(backend) & shape) != 0;
      assert((flutter::ValidateAvioAntialiasingBackend(config, backend) ==
              nullptr) == implemented);
    }
    config.continuous_requested_classes = uint64_t{1} << 63;
    assert(flutter::ValidateAvioAntialiasingConfig(&config, kAA, true));
    assert(flutter::ValidateAvioAntialiasingBackend(config, backend));
    config.continuous_requested_classes = 0;
  }
  config.policy = kFlutterAvioAntialiasingPolicyMsaa4;
  config.layer_sample_count = 4;
  config.coverage_region_max_bytes = config.layer_region_max_bytes = 0;
  config.continuous_requested_classes = 1;
  assert(flutter::ValidateAvioAntialiasingConfig(&config, kAA, true));
}

void CapabilityRowsHonorExactAppendBoundsWithoutAllocation() {
  constexpr uint64_t unknown = uint64_t{1} << 63;
  const size_t before = heap_allocations;
  track_heap = true;
  for (size_t size : {offsetof(FlutterAvioExtensionCapabilities,
                               continuous_supported_classes),
                      offsetof(FlutterAvioExtensionCapabilities,
                               continuous_supported_classes_vulkan),
                      offsetof(FlutterAvioExtensionCapabilities,
                               continuous_supported_classes_gles),
                      offsetof(FlutterAvioExtensionCapabilities,
                               continuous_supported_classes_metal),
                      sizeof(FlutterAvioExtensionCapabilities)}) {
    FlutterAvioExtensionCapabilities caps = {};
    caps.struct_size = size;
    caps.continuous_supported_classes = unknown;
    caps.continuous_supported_classes_vulkan = unknown;
    caps.continuous_supported_classes_gles = unknown;
    caps.continuous_supported_classes_metal = unknown;
    flutter::WriteAvioContinuousCapabilities(caps, 0x7f, 0, 0);
    const bool union_available =
        size > offsetof(FlutterAvioExtensionCapabilities,
                        continuous_supported_classes);
    const bool vk_available =
        size > offsetof(FlutterAvioExtensionCapabilities,
                        continuous_supported_classes_vulkan);
    const bool gl_available =
        size > offsetof(FlutterAvioExtensionCapabilities,
                        continuous_supported_classes_gles);
    const bool metal_available =
        size > offsetof(FlutterAvioExtensionCapabilities,
                        continuous_supported_classes_metal);
    assert(caps.continuous_supported_classes ==
           (union_available ? 0x7f : unknown));
    assert(caps.continuous_supported_classes_vulkan ==
           (vk_available ? 0x7f : unknown));
    assert(caps.continuous_supported_classes_gles ==
           (gl_available ? 0 : unknown));
    assert(caps.continuous_supported_classes_metal ==
           (metal_available ? 0 : unknown));
  }
  FlutterAvioExtensionCapabilities future = {};
  future.struct_size = sizeof(future);
  flutter::WriteAvioContinuousCapabilities(future, 0x7f, unknown, 0);
  assert(future.continuous_supported_classes == (0x7f | unknown));
  assert(future.continuous_supported_classes_gles == unknown);
  track_heap = false;
  assert(heap_allocations == before);
}

void RootFrameFactsStayBorrowedValidatedAndAllocationFree() {
  const size_t before = heap_allocations;
  track_heap = true;
  flutter::AvioFrameFacts facts;
  assert(facts.IsValid() && !facts.HasMetadata());
  flutter::EmbedderAvioFrameFacts absent(facts);
  assert(!absent.effect() && !absent.ground());
  facts.ready_content_revision = 17u;
  facts.item_effect_declaration_id = 1;
  facts.item_opacity = 0.25;
  facts.ground_authored = true;
  facts.ground_color_argb = 0xFF123456u;
  flutter::EmbedderAvioFrameFacts authored(facts);
  assert(authored.ready_content()->content_revision == 17u);
  assert(authored.ready_content()->kind == kFlutterAvioReadyContentKindStatic);
  facts.ready_content_kind = flutter::AvioReadyContentKind::kLive;
  flutter::EmbedderAvioFrameFacts live(facts);
  assert(live.ready_content()->kind == kFlutterAvioReadyContentKindLive);
  assert(authored.ready_content()->kind == kFlutterAvioReadyContentKindStatic);
  facts.ready_content_kind = static_cast<flutter::AvioReadyContentKind>(2u);
  assert(!facts.IsValid());
  flutter::EmbedderAvioFrameFacts invalid_kind(facts);
  assert(!invalid_kind.ready_content());
  facts.ready_content_kind = flutter::AvioReadyContentKind::kStatic;
  assert(authored.effect()->struct_size == sizeof(FlutterAvioItemEffect));
  assert(authored.effect()->opacity == 0.25);
  assert(authored.effect()->declaration_id == 1u);
  assert(authored.ground()->struct_size == sizeof(FlutterAvioOutputGround));
  assert(authored.ground()->has_color &&
         authored.ground()->color_argb == 0xFF123456u);
  facts.ground_color_argb.reset();
  flutter::EmbedderAvioFrameFacts clear(facts);
  assert(clear.ground() && !clear.ground()->has_color);
  assert(authored.ground()->has_color);  // Separate revision copies.
  for (double invalid : {-0.01, 1.01, std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
    facts.item_opacity = invalid;
    assert(!facts.IsValid());
    flutter::EmbedderAvioFrameFacts rejected(facts);
    assert(!rejected.effect() && !rejected.ground());
  }
  facts = {};
  facts.ground_color_argb = 0u;
  assert(!facts.IsValid());
  facts = {};
  facts.ready_content_revision = 0u;
  assert(!facts.IsValid());
  facts = {};
  facts.invalid = true;
  assert(!facts.IsValid() && facts.HasMetadata());
  track_heap = false;
  assert(heap_allocations == before);
}

void SplitGroundRegionsStayBoundedExactAndFailClosed() {
  flutter::AvioFrameFacts facts;
  facts.ground_authored = true;
  facts.ground_regions_count = 4;
  for (size_t i = 0; i < 4; ++i) {
    facts.ground_regions[i] = {double(i * 100), 0, double((i + 1) * 100), 200,
                               uint32_t(0xFF123456u + i)};
  }
  assert(facts.IsValidForRoot(400, 200));
  const size_t before = heap_allocations;
  track_heap = true;
  flutter::EmbedderAvioFrameFacts copy(facts);
  assert(copy.ground()->regions_count == 4 && copy.ground()->regions);
  assert(copy.ground()->regions[3].rect.left == 300 &&
         copy.ground()->regions[3].rect.right == 400 &&
         copy.ground()->regions[3].color_argb == 0xFF123459u);
  track_heap = false;
  assert(heap_allocations == before);
  assert(!facts.IsValidForRoot(399, 200));
  facts.ground_regions[1].left = 99;
  assert(!facts.IsValid());
  facts.ground_regions[1].left = 100;
  facts.ground_regions[0].left = -1;
  assert(!facts.IsValidForRoot(400, 200));
  facts.ground_regions[0].left = 0;
  facts.ground_regions[0].bottom = std::numeric_limits<double>::quiet_NaN();
  assert(!facts.IsValid());
  facts.ground_regions[0].bottom = 200;
  facts.ground_color_argb = 0;
  assert(!facts.IsValid());
  facts.ground_color_argb.reset();
  facts.ground_regions_count = 5;
  assert(!facts.IsValid());
  flutter::EmbedderAvioFrameFacts rejected(facts);
  assert(!rejected.ground());
}

void PartialBackendDescriptorsPreserveAvailabilityWithoutInventingBytes() {
  impeller::AvioRenderResourceReport source;
  source.available = true;
  impeller::AvioRenderResourceEntry entry;
  entry.kind_id = 0xFEEDu;
  entry.usage.entries = 1;
  entry.usage.nominal_bytes = 1048576;
  entry.fields_supported = impeller::kAvioResourceFieldCounts |
                           impeller::kAvioResourceFieldDescriptorBytes |
                           impeller::kAvioResourceFieldTextureDescriptor |
                           (uint64_t{1} << 63);
  entry.unsupported_reason_id = 0xABCDEFu;
  entry.descriptor_width = 256;
  entry.descriptor_height = 256;
  entry.descriptor_sample_count = 4;
  entry.descriptor_format_id = 0x123456u;
  source.AddEntry(entry);
  impeller::AvioRenderResourceReport merged;
  merged.Merge(source);
  assert(merged.entries[0].fields_supported == entry.fields_supported);
  const size_t before = heap_allocations;
  track_heap = true;
  flutter::DeliverAvioRenderResourceReport(
      [](const FlutterAvioRenderResourceReport* r, void*) {
        const auto& e = r->entries[0];
        assert(e.kind_id == 0xFEEDu && e.nominal_bytes == 1048576);
        assert((e.fields_supported &
                kFlutterAvioResourceFieldActualAllocatedBytes) == 0);
        assert(e.real_bytes == 0);  // Unavailable storage, never measured zero.
        assert(e.fields_supported == (11u | (uint64_t{1} << 63)));
        assert(e.unsupported_reason_id == 0xABCDEFu);
        assert(e.descriptor_width == 256 && e.descriptor_height == 256);
        assert(e.descriptor_sample_count == 4 &&
               e.descriptor_format_id == 0x123456u);
      },
      nullptr, merged, kFlutterAvioRenderResourceReportSuccess);
  track_heap = false;
  assert(heap_allocations == before);
}

void RootFrameFactNegotiationRequiresExactRootOpportunity() {
  constexpr auto prerequisites =
      kFlutterAvioExtensionFeatureRootRenderTarget |
      kFlutterAvioExtensionFeatureFrameOpportunityOutcomes;
  for (auto feature : {kFlutterAvioExtensionFeatureEmptyFrame,
                       kFlutterAvioExtensionFeatureItemEffects,
                       kFlutterAvioExtensionFeatureOutputGround,
                       kFlutterAvioExtensionFeatureReadyContent}) {
    assert(flutter::ValidateAvioFrameFactFeatures(feature));
    assert(flutter::ValidateAvioFrameFactFeatures(
        feature | kFlutterAvioExtensionFeatureRootRenderTarget));
    assert(flutter::ValidateAvioFrameFactFeatures(
        feature | kFlutterAvioExtensionFeatureFrameOpportunityOutcomes));
    assert(!flutter::ValidateAvioFrameFactFeatures(feature | prerequisites));
  }
  assert(!flutter::ValidateAvioFrameFactFeatures(0));
  static_assert(kFlutterPresentRenderTargetStatusEmptyContent == 8);
  static_assert(kFlutterPresentRenderTargetStatusInvalidFrameFacts == 9);
  static_assert(
      offsetof(FlutterPresentRenderTargetInfo, item_effect) >
      offsetof(FlutterPresentRenderTargetInfo, window_previews_invalid));
  static_assert(offsetof(FlutterPresentViewInfo, item_effect) >
                offsetof(FlutterPresentViewInfo, compositor_materials_invalid));
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
  static_assert(FLUTTER_AVIO_EXTENSION_VERSION == 9);
  static_assert(kFlutterAvioResourceFieldDescriptorMultiplicity == 16);
  static_assert(kFlutterAvioResourceFieldLeases == 32);
  static_assert(kFlutterAvioContinuousKnownClasses ==
                impeller::kAvioContinuousKnownClasses);
  static_assert(
      kFlutterAvioContinuousClassImageEdge ==
      static_cast<uint64_t>(impeller::AvioContinuousClass::kImageEdge));
  static_assert(impeller::kAvioContinuousSupportedClasses == 0x7f);
  static_assert(impeller::AvioContinuousSupportedClasses(
                    impeller::AvioCoverageBackend::kGLES) == 0);
  static_assert(impeller::AvioContinuousSupportedClasses(
                    impeller::AvioCoverageBackend::kMetal) == 0);
  static_assert(kFlutterAvioRenderResourceDeviceBuffers == 9);
  static_assert(offsetof(FlutterProjectArgs, avio_antialiasing_config) >
                offsetof(FlutterProjectArgs, avio_resource_lifecycle_config));
  static_assert(
      offsetof(FlutterEngineProcTable, RequestAvioRenderResourceReport) >
      offsetof(FlutterEngineProcTable, SetAvioViewVisibility));
  CapabilityRowsHonorExactAppendBoundsWithoutAllocation();
  ContinuousClassAdmissionIsBackendSpecific();
  RootFrameFactsStayBorrowedValidatedAndAllocationFree();
  RootFrameFactNegotiationRequiresExactRootOpportunity();
  SplitGroundRegionsStayBoundedExactAndFailClosed();
  PartialBackendDescriptorsPreserveAvailabilityWithoutInventingBytes();
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
      "Avio coverage ABI/report production contracts: 16 passed, 0 skipped");
  std::printf(
      "Production readiness: coverage=%s, Vulkan_continuous_classes=0x%llx\n",
      impeller::kAvioCoveragePolicyImplemented ? "enabled" : "disabled",
      static_cast<unsigned long long>(
          impeller::kAvioContinuousSupportedClasses));
}
