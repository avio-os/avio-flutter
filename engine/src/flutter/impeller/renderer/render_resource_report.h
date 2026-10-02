// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_RENDER_RESOURCE_REPORT_H_
#define FLUTTER_IMPELLER_RENDERER_RENDER_RESOURCE_REPORT_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "impeller/renderer/render_resource_usage.h"

namespace impeller {

// Stable transport IDs. Unknown future IDs remain reportable verbatim.
enum class AvioRenderResourceKind : uint32_t {
  kTransientAttachments = 1,
  kOffscreens = 2,
  kFlipTargets = 3,
  kImageTextures = 4,
  kGlyphAtlases = 5,
  kPipelines = 6,
  kCoverageRegion = 7,
  kLayerRegion = 8,
  kDeviceBuffers = 9,
};

inline constexpr uint64_t kAvioCounterRasterThreadAllocations = 1u << 0;
inline constexpr uint64_t kAvioCounterFirstUseCompiles = 1u << 1;
inline constexpr uint64_t kAvioCounterGlyphAtlasGrowths = 1u << 2;
inline constexpr uint64_t kAvioCounterImageUploads = 1u << 3;
inline constexpr uint64_t kAvioCounterSnapshotAllocations = 1u << 4;
inline constexpr uint64_t kAvioCounterCoverageFlushes = 1u << 5;
// One availability bit covers overflow event count and allocated real bytes.
inline constexpr uint64_t kAvioCounterLayerRegionOverflows = 1u << 6;

inline constexpr uint64_t kAvioResourceFieldCounts = 1u << 0;
inline constexpr uint64_t kAvioResourceFieldDescriptorBytes = 1u << 1;
inline constexpr uint64_t kAvioResourceFieldActualAllocatedBytes = 1u << 2;
inline constexpr uint64_t kAvioResourceFieldTextureDescriptor = 1u << 3;
inline constexpr uint64_t kAvioResourceFieldDescriptorMultiplicity = 1u << 4;
inline constexpr uint64_t kAvioResourceFieldLeases = 1u << 5;
inline constexpr uint32_t kAvioResourceReasonPhysicalAllocationUnavailable = 1u;

struct AvioRenderResourceEntry {
  uint32_t kind_id = 0u;
  RenderResourceUsage usage;
  uint64_t fields_supported = kAvioResourceFieldCounts |
                              kAvioResourceFieldDescriptorBytes |
                              kAvioResourceFieldActualAllocatedBytes;
  uint32_t unsupported_reason_id = 0u;
  uint32_t descriptor_width = 0u;
  uint32_t descriptor_height = 0u;
  uint32_t descriptor_sample_count = 0u;
  uint32_t descriptor_format_id = 0u;
};

struct AvioCoverageReasonUsage {
  uint32_t reason_id = 0u;
  uint64_t draw_count = 0u;
  uint64_t pixel_area = 0u;
};

enum class AvioCoverageReason : uint32_t {
  kNativeColourIsland = 1u,
  kCachedFillMask = 2u,
  kCachedClipMask = 3u,
  kAnalyticQuadClip = 4u,
  // DisplayList pre-pass facts, distinct from the execution routes above.
  kClassPathFill = 16u,
  kClassPathStroke = 17u,
  kClassFractionalClip = 18u,
  kClassNonRectClip = 19u,
  kClassArc = 20u,
  kClassBorder = 21u,
  kClassEllipticalRoundRect = 22u,
  kClassRejectedBlend = 23u,
  kClassNoAntialias = 24u,
  kClassImageEdge = 25u,
  kClassRejectedSource = 26u,
  kClassDestinationRead = 27u,
  // Peak simultaneous nominal colour pixels, never real allocated bytes.
  kClassLayerPeakDemand = 28u,
  // Classification incomplete; conservative rendering remains mandatory.
  kClassCapacityOverflow = 29u,
  kClassStorageUnavailable = 30u,
};

// Fixed storage: reading this report never grows a container on the raster
// thread. Only implemented kinds/counters are present. Absent is not zero.
struct AvioRenderResourceReport {
  static constexpr size_t kMaxEntries = 64u;
  static constexpr size_t kMaxCoverageReasons = 32u;
  std::array<AvioRenderResourceEntry, kMaxEntries> entries = {};
  size_t entries_count = 0u;
  std::array<AvioCoverageReasonUsage, kMaxCoverageReasons> coverage_reasons =
      {};
  size_t coverage_reasons_count = 0u;
  bool available = false;
  bool truncated = false;
  uint64_t counters_supported = 0u;
  uint64_t raster_thread_allocations = 0u;
  uint64_t first_use_compiles = 0u;
  uint64_t glyph_atlas_growths = 0u;
  uint64_t image_uploads = 0u;
  uint64_t snapshot_allocations = 0u;
  // Successful encoded native color-island tile-to-parent composites. This
  // excludes atlas resets and direct 1x passes; it is not GPU completion.
  uint64_t coverage_flushes = 0u;
  uint64_t layer_region_overflows = 0u;
  uint64_t layer_region_overflow_real_bytes = 0u;

  constexpr void Merge(const AvioRenderResourceReport& other) {
    available = available || other.available;
    truncated = truncated || other.truncated ||
                other.entries_count > other.entries.size() ||
                other.coverage_reasons_count > other.coverage_reasons.size();
    for (size_t i = 0; i < other.entries_count && i < other.entries.size();
         i++) {
      AddEntry(other.entries[i]);
    }
    for (size_t i = 0;
         i < other.coverage_reasons_count && i < other.coverage_reasons.size();
         i++) {
      AddCoverageReason(other.coverage_reasons[i]);
    }
    if (other.counters_supported & kAvioCounterRasterThreadAllocations) {
      raster_thread_allocations += other.raster_thread_allocations;
    }
    if (other.counters_supported & kAvioCounterFirstUseCompiles) {
      first_use_compiles += other.first_use_compiles;
    }
    if (other.counters_supported & kAvioCounterGlyphAtlasGrowths) {
      glyph_atlas_growths += other.glyph_atlas_growths;
    }
    if (other.counters_supported & kAvioCounterImageUploads) {
      image_uploads += other.image_uploads;
    }
    if (other.counters_supported & kAvioCounterSnapshotAllocations) {
      snapshot_allocations += other.snapshot_allocations;
    }
    if (other.counters_supported & kAvioCounterCoverageFlushes) {
      coverage_flushes += other.coverage_flushes;
    }
    if (other.counters_supported & kAvioCounterLayerRegionOverflows) {
      layer_region_overflows += other.layer_region_overflows;
      layer_region_overflow_real_bytes +=
          other.layer_region_overflow_real_bytes;
    }
    counters_supported |= other.counters_supported;
  }

  constexpr bool AddEntry(AvioRenderResourceEntry entry) {
    if (entries_count >= entries.size()) {
      truncated = true;
      return false;
    }
    entries[entries_count++] = entry;
    return true;
  }
  constexpr bool AddEntry(uint32_t kind_id, RenderResourceUsage usage) {
    if (entries_count >= entries.size()) {
      truncated = true;
      return false;
    }
    entries[entries_count++] = {kind_id, usage};
    return true;
  }
  constexpr bool AddEntry(AvioRenderResourceKind kind,
                          RenderResourceUsage usage) {
    return AddEntry(static_cast<uint32_t>(kind), usage);
  }
  constexpr bool AddCoverageReason(AvioCoverageReasonUsage reason) {
    if (coverage_reasons_count >= coverage_reasons.size()) {
      truncated = true;
      return false;
    }
    coverage_reasons[coverage_reasons_count++] = reason;
    return true;
  }
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_RENDER_RESOURCE_REPORT_H_
