// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_ALLOCATED_BUFFER_LEDGER_H_
#define FLUTTER_IMPELLER_RENDERER_ALLOCATED_BUFFER_LEDGER_H_

#include <memory>
#include <mutex>
#include <utility>

#include "impeller/renderer/render_resource_report.h"
#include "impeller/renderer/render_resource_scope.h"

namespace impeller {

// One cold ledger per allocator. The guard lives in the deferred native buffer
// resource, so GPU/command-buffer owners keep both the bytes and census live.
// No extra heap object or descriptor-key map is allocated at registration.
class AllocatedBufferLedger final
    : public std::enable_shared_from_this<AllocatedBufferLedger> {
 public:
  class Registration final {
   public:
    Registration() = default;
    ~Registration() { Reset(); }
    Registration(Registration&& other) noexcept
        : ledger_(std::move(other.ledger_)),
          nominal_(other.nominal_),
          real_(other.real_) {}
    Registration& operator=(Registration&& other) noexcept {
      if (this != &other) {
        Reset();
        ledger_ = std::move(other.ledger_);
        nominal_ = other.nominal_;
        real_ = other.real_;
      }
      return *this;
    }
    Registration(const Registration&) = delete;
    Registration& operator=(const Registration&) = delete;

   private:
    friend class AllocatedBufferLedger;
    Registration(std::shared_ptr<AllocatedBufferLedger> ledger,
                 size_t nominal,
                 size_t real)
        : ledger_(std::move(ledger)), nominal_(nominal), real_(real) {}
    void Reset() {
      // Retain the ledger until after its mutex is released. No native resource
      // destructor or shared-owner callback runs while that mutex is held.
      auto ledger = std::move(ledger_);
      if (ledger) {
        std::scoped_lock lock(ledger->mutex_);
        auto& usage = ledger->usage_;
        usage.entries--;
        usage.nominal_bytes -= nominal_;
        usage.real_bytes -= real_;
        usage.orphans_released_entries++;
        usage.orphans_released_real_bytes += real_;
      }
    }
    std::shared_ptr<AllocatedBufferLedger> ledger_;
    size_t nominal_ = 0u;
    size_t real_ = 0u;
  };

  // Called only after vmaCreateBuffer succeeds; real is VmaAllocationInfo.size,
  // not a descriptor estimate or the size of VMA's containing memory block.
  Registration Register(
      size_t nominal,
      size_t real,
      bool raster_frame,
      AvioRasterAllocationCause cause = AvioRasterAllocationCause::kFrameWork) {
    auto owner = shared_from_this();
    {
      std::scoped_lock lock(mutex_);
      usage_.entries++;
      usage_.nominal_bytes += nominal;
      usage_.real_bytes += real;
      usage_.created_entries++;
      usage_.created_real_bytes += real;
      raster_allocations_ +=
          raster_frame && cause == AvioRasterAllocationCause::kFrameWork;
      snapshot_allocations_ +=
          raster_frame && cause == AvioRasterAllocationCause::kSnapshot;
    }
    return Registration(std::move(owner), nominal, real);
  }

  AvioRenderResourceReport Report(bool start_new_interval) {
    std::scoped_lock lock(mutex_);
    AvioRenderResourceReport report;
    report.available = true;
    AvioRenderResourceEntry entry;
    entry.kind_id =
        static_cast<uint32_t>(AvioRenderResourceKind::kDeviceBuffers);
    entry.usage = usage_;
    entry.fields_supported = kAvioResourceFieldCounts |
                             kAvioResourceFieldDescriptorBytes |
                             kAvioResourceFieldActualAllocatedBytes;
    report.AddEntry(entry);
    // Together with the allocated-image provider this counts successful native
    // image and buffer allocation events originating within a raster frame.
    report.counters_supported =
        kAvioCounterRasterThreadAllocations | kAvioCounterSnapshotAllocations;
    report.raster_thread_allocations = raster_allocations_;
    report.snapshot_allocations = snapshot_allocations_;
    if (start_new_interval) {
      usage_.created_entries = usage_.created_real_bytes = 0u;
      usage_.orphans_released_entries = usage_.orphans_released_real_bytes = 0u;
      raster_allocations_ = snapshot_allocations_ = 0u;
    }
    return report;
  }

 private:
  std::mutex mutex_;
  RenderResourceUsage usage_;
  uint64_t raster_allocations_ = 0u;
  uint64_t snapshot_allocations_ = 0u;
};

}  // namespace impeller
#endif  // FLUTTER_IMPELLER_RENDERER_ALLOCATED_BUFFER_LEDGER_H_
