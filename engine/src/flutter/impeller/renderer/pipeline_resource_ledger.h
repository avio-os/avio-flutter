// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_PIPELINE_RESOURCE_LEDGER_H_
#define FLUTTER_IMPELLER_RENDERER_PIPELINE_RESOURCE_LEDGER_H_

#include <memory>
#include <mutex>
#include <utility>

#include "impeller/renderer/render_resource_report.h"
#include "impeller/renderer/render_resource_scope.h"

namespace impeller {
// Capture at the cache-miss request, before a compile job changes threads.
// Native creation and interval reporting never reconstruct caller provenance
// from the compiler thread's TLS or from descriptor/resource-kind guesses.
struct AvioPipelineCreationOrigin {
  bool raster_frame = false;
  AvioRasterAllocationCause cause = AvioRasterAllocationCause::kFrameWork;
  static AvioPipelineCreationOrigin Capture() {
    return {IsAvioRasterFrameActive(), GetAvioRasterAllocationCause()};
  }
};

// The native driver does not expose allocation sizes for VkPipeline. This
// ledger counts successful native objects, including sampler variants and
// objects still held by submitted work after cache eviction. Byte fields and
// cache-key fields are unavailable for this kind; they remain zero.
class PipelineResourceLedger final {
 public:
  class Registration final {
   public:
    Registration() = default;
    Registration(std::shared_ptr<PipelineResourceLedger> ledger,
                 AvioPipelineCreationOrigin origin)
        : ledger_(std::move(ledger)) {
      if (ledger_) {
        ledger_->Created(origin);
      }
    }
    ~Registration() { Release(); }
    Registration(Registration&& other) noexcept
        : ledger_(std::move(other.ledger_)) {}
    Registration& operator=(Registration&& other) noexcept {
      if (this != &other) {
        Release();
        ledger_ = std::move(other.ledger_);
      }
      return *this;
    }
    Registration(const Registration&) = delete;
    Registration& operator=(const Registration&) = delete;

   private:
    void Release() {
      if (ledger_) {
        ledger_->Destroyed();
        ledger_.reset();
      }
    }
    std::shared_ptr<PipelineResourceLedger> ledger_;
  };

  AvioRenderResourceReport Report(bool start_new_interval) {
    std::lock_guard lock(mutex_);
    AvioRenderResourceReport report;
    report.available = true;
    RenderResourceUsage usage;
    usage.entries = live_;
    usage.created_entries = created_;
    report.AddEntry(
        {.kind_id = static_cast<uint32_t>(AvioRenderResourceKind::kPipelines),
         .usage = usage,
         .fields_supported = kAvioResourceFieldCounts,
         .unsupported_reason_id =
             kAvioResourceReasonPhysicalAllocationUnavailable});
    report.counters_supported = kAvioCounterFirstUseCompiles |
                                kAvioCounterRasterThreadAllocations |
                                kAvioCounterSnapshotAllocations;
    report.raster_thread_allocations = raster_allocations_;
    report.snapshot_allocations = snapshot_allocations_;
    report.first_use_compiles = first_use_;
    if (start_new_interval) {
      created_ = 0u;
      first_use_ = 0u;
      raster_allocations_ = snapshot_allocations_ = 0u;
    }
    return report;
  }

 private:
  void Created(AvioPipelineCreationOrigin origin) {
    std::lock_guard lock(mutex_);
    live_++;
    created_++;
    first_use_ += origin.raster_frame;
    if (origin.raster_frame) {
      raster_allocations_ +=
          origin.cause == AvioRasterAllocationCause::kFrameWork;
      snapshot_allocations_ +=
          origin.cause == AvioRasterAllocationCause::kSnapshot;
    }
  }
  void Destroyed() {
    std::lock_guard lock(mutex_);
    live_--;
  }
  std::mutex mutex_;
  size_t live_ = 0u;
  size_t created_ = 0u;
  uint64_t first_use_ = 0u;
  uint64_t raster_allocations_ = 0u;
  uint64_t snapshot_allocations_ = 0u;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_PIPELINE_RESOURCE_LEDGER_H_
