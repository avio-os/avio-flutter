// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_PIPELINE_RESOURCE_LEDGER_H_
#define FLUTTER_IMPELLER_RENDERER_PIPELINE_RESOURCE_LEDGER_H_

#include <memory>
#include <mutex>
#include <utility>

#include "impeller/renderer/render_resource_report.h"

namespace impeller {

// The native driver does not expose allocation sizes for VkPipeline. This
// ledger counts successful native objects, including sampler variants and
// objects still held by submitted work after cache eviction. Byte fields and
// cache-key fields are unavailable for this kind; they remain zero.
class PipelineResourceLedger final {
 public:
  class Registration final {
   public:
    Registration() = default;
    Registration(std::shared_ptr<PipelineResourceLedger> ledger, bool first_use)
        : ledger_(std::move(ledger)) {
      if (ledger_) {
        ledger_->Created(first_use);
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
    report.AddEntry(AvioRenderResourceKind::kPipelines, usage);
    report.counters_supported = kAvioCounterFirstUseCompiles;
    report.first_use_compiles = first_use_;
    if (start_new_interval) {
      created_ = 0u;
      first_use_ = 0u;
    }
    return report;
  }

 private:
  void Created(bool first_use) {
    std::lock_guard lock(mutex_);
    live_++;
    created_++;
    first_use_ += first_use;
  }
  void Destroyed() {
    std::lock_guard lock(mutex_);
    live_--;
  }
  std::mutex mutex_;
  size_t live_ = 0u;
  size_t created_ = 0u;
  uint64_t first_use_ = 0u;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_PIPELINE_RESOURCE_LEDGER_H_
