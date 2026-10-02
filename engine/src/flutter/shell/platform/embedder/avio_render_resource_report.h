// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_AVIO_RENDER_RESOURCE_REPORT_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_AVIO_RENDER_RESOURCE_REPORT_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <mutex>

#include "flutter/shell/platform/embedder/embedder.h"
#include "impeller/renderer/render_resource_report.h"

namespace flutter {

// Convert only during the callback. No allocation and no filtering of IDs.
inline void DeliverAvioRenderResourceReport(
    FlutterAvioRenderResourceReportCallback callback,
    void* user_data,
    const impeller::AvioRenderResourceReport& source,
    FlutterAvioRenderResourceReportStatus status) {
  std::array<FlutterAvioRenderResourceEntry,
             FLUTTER_AVIO_MAX_RENDER_RESOURCE_ENTRIES>
      entries = {};
  std::array<FlutterAvioCoverageReasonUsage, FLUTTER_AVIO_MAX_COVERAGE_REASONS>
      reasons = {};
  const size_t count = std::min(source.entries_count, entries.size());
  const size_t reason_count =
      std::min(source.coverage_reasons_count, reasons.size());
  if (source.truncated || count != source.entries_count ||
      reason_count != source.coverage_reasons_count) {
    status = kFlutterAvioRenderResourceReportTruncated;
  }
  for (size_t i = 0; i < count; i++) {
    const auto& entry = source.entries[i];
    const auto& u = entry.usage;
    entries[i] = {sizeof(FlutterAvioRenderResourceEntry),
                  entry.kind_id,
                  u.entries,
                  u.nominal_bytes,
                  u.real_bytes,
                  u.leased_entries,
                  u.peak_leased_nominal_bytes,
                  u.distinct_keys,
                  u.duplicate_entries,
                  u.orphans_released_entries,
                  u.orphans_released_real_bytes,
                  u.created_entries,
                  u.created_real_bytes};
  }
  for (size_t i = 0; i < reason_count; i++) {
    const auto& reason = source.coverage_reasons[i];
    reasons[i] = {sizeof(FlutterAvioCoverageReasonUsage), reason.reason_id,
                  reason.draw_count, reason.pixel_area};
  }
  const FlutterAvioRenderResourceReport report = {
      sizeof(FlutterAvioRenderResourceReport),
      status,
      count,
      count == 0 ? nullptr : entries.data(),
      source.counters_supported,
      source.raster_thread_allocations,
      source.first_use_compiles,
      source.glyph_atlas_growths,
      source.image_uploads,
      source.snapshot_allocations,
      reason_count,
      reason_count == 0 ? nullptr : reasons.data(),
      source.coverage_flushes,
      source.layer_region_overflows,
      source.layer_region_overflow_real_bytes};
  callback(&report, user_data);
}

// Request custody outlives EmbedderEngine in an already posted task. Close
// runs on the raster runner before shell/thread-host teardown; later tasks
// cannot deliver a second callback. Callback code always runs outside locks.
class AvioRenderResourceReportRequests {
 public:
  uint64_t Reserve(FlutterAvioRenderResourceReportCallback callback,
                   void* user_data) {
    std::scoped_lock lock(mutex_);
    if (closed_ || !callback ||
        next_ticket_ == std::numeric_limits<uint64_t>::max()) {
      return 0;
    }
    for (auto& slot : pending_) {
      if (slot.ticket == 0) {
        const uint64_t ticket = ++next_ticket_;
        slot = {ticket, callback, user_data};
        return ticket;
      }
    }
    return 0;
  }

  void Complete(uint64_t ticket,
                const impeller::AvioRenderResourceReport& report,
                FlutterAvioRenderResourceReportStatus status) {
    Pending completion;
    {
      std::scoped_lock lock(mutex_);
      for (auto& slot : pending_) {
        if (slot.ticket == ticket && ticket != 0) {
          completion = slot;
          slot = {};
          break;
        }
      }
    }
    if (completion.callback) {
      DeliverAvioRenderResourceReport(completion.callback, completion.user_data,
                                      report, status);
    }
  }

  void Close() {
    decltype(pending_) cancelled;
    {
      std::scoped_lock lock(mutex_);
      closed_ = true;
      cancelled = pending_;
      pending_ = {};
    }
    for (const auto& request : cancelled) {
      if (request.callback) {
        DeliverAvioRenderResourceReport(
            request.callback, request.user_data, {},
            kFlutterAvioRenderResourceReportEngineUnavailable);
      }
    }
  }

 private:
  struct Pending {
    uint64_t ticket = 0;
    FlutterAvioRenderResourceReportCallback callback = nullptr;
    void* user_data = nullptr;
  };
  std::mutex mutex_;
  std::array<Pending, FLUTTER_AVIO_MAX_RENDER_RESOURCE_ENTRIES> pending_ = {};
  uint64_t next_ticket_ = 0;
  bool closed_ = false;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_EMBEDDER_AVIO_RENDER_RESOURCE_REPORT_H_
