// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/allocated_image_ledger.h"

#include <utility>

namespace impeller {

size_t AllocatedImageLedger::KindIndex(AvioRenderResourceKind kind) {
  const auto value = static_cast<uint32_t>(kind);
  // All production construction scopes use the stable known image classes.
  // A malformed scope remains counted as an image, never loses its bytes.
  return value > 0u && value <= kKindCount
             ? value - 1u
             : static_cast<size_t>(AvioRenderResourceKind::kImageTextures) - 1u;
}

AllocatedImageLedger::Registration::Registration(
    std::shared_ptr<AllocatedImageLedger> ledger,
    AvioRenderResourceKind kind,
    AvioAllocatedImageKey key,
    size_t nominal_bytes,
    size_t real_bytes,
    bool raster_frame)
    : ledger_(std::move(ledger)),
      kind_(static_cast<AvioRenderResourceKind>(KindIndex(kind) + 1u)),
      key_(key),
      nominal_(nominal_bytes),
      real_(real_bytes) {
  ledger_->Add(*this, raster_frame);
}

AllocatedImageLedger::Registration::~Registration() {
  Reset();
}

void AllocatedImageLedger::Registration::Reset() {
  if (ledger_) {
    ledger_->Remove(*this);
    ledger_.reset();
  }
}

void AllocatedImageLedger::Registration::RecordImageUpload(
    bool raster_frame) const {
  if (ledger_) {
    ledger_->RecordImageUpload(kind_, raster_frame);
  }
}

AllocatedImageLedger::Registration::Registration(Registration&& other) noexcept
    : ledger_(std::move(other.ledger_)),
      kind_(other.kind_),
      key_(other.key_),
      nominal_(other.nominal_),
      real_(other.real_) {
  if (ledger_) {
    ledger_->Replace(other, *this);
  }
}

AllocatedImageLedger::Registration&
AllocatedImageLedger::Registration::operator=(Registration&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  Reset();
  ledger_ = std::move(other.ledger_);
  kind_ = other.kind_;
  key_ = other.key_;
  nominal_ = other.nominal_;
  real_ = other.real_;
  if (ledger_) {
    ledger_->Replace(other, *this);
  }
  return *this;
}

AllocatedImageLedger::Registration AllocatedImageLedger::Register(
    AvioRenderResourceKind kind,
    AvioAllocatedImageKey key,
    size_t nominal_bytes,
    size_t real_bytes,
    bool raster_frame) {
  return Registration(shared_from_this(), kind, key, nominal_bytes, real_bytes,
                      raster_frame);
}

void AllocatedImageLedger::Add(Registration& registration, bool raster_frame) {
  std::scoped_lock lock(mutex_);
  const size_t index = KindIndex(registration.kind_);
  auto& usage = usage_[index];
  bool duplicate = false;
  for (auto* entry = first_; entry; entry = entry->next_) {
    if (entry->kind_ == registration.kind_ &&
        entry->key_ == registration.key_) {
      duplicate = true;
      break;
    }
  }
  registration.next_ = first_;
  if (first_) {
    first_->previous_ = &registration;
  }
  first_ = &registration;
  usage.entries++;
  usage.nominal_bytes += registration.nominal_;
  usage.real_bytes += registration.real_;
  usage.distinct_keys += !duplicate;
  usage.duplicate_entries += duplicate;
  usage.created_entries++;
  usage.created_real_bytes += registration.real_;
  observed_[index] = true;
  if (raster_frame) {
    raster_allocations_++;
    if (registration.kind_ == AvioRenderResourceKind::kOffscreens ||
        registration.kind_ == AvioRenderResourceKind::kFlipTargets ||
        registration.kind_ == AvioRenderResourceKind::kLayerRegion) {
      snapshot_allocations_++;
    }
  }
}

void AllocatedImageLedger::Remove(Registration& registration) {
  std::scoped_lock lock(mutex_);
  if (registration.previous_) {
    registration.previous_->next_ = registration.next_;
  } else {
    first_ = registration.next_;
  }
  if (registration.next_) {
    registration.next_->previous_ = registration.previous_;
  }
  auto& usage = usage_[KindIndex(registration.kind_)];
  bool duplicate = false;
  for (auto* entry = first_; entry; entry = entry->next_) {
    if (entry->kind_ == registration.kind_ &&
        entry->key_ == registration.key_) {
      duplicate = true;
      break;
    }
  }
  usage.entries--;
  usage.nominal_bytes -= registration.nominal_;
  usage.real_bytes -= registration.real_;
  usage.distinct_keys -= !duplicate;
  usage.duplicate_entries -= duplicate;
  usage.orphans_released_entries++;
  usage.orphans_released_real_bytes += registration.real_;
  registration.previous_ = registration.next_ = nullptr;
}

void AllocatedImageLedger::Replace(Registration& from, Registration& to) {
  std::scoped_lock lock(mutex_);
  to.previous_ = from.previous_;
  to.next_ = from.next_;
  if (to.previous_) {
    to.previous_->next_ = &to;
  } else {
    first_ = &to;
  }
  if (to.next_) {
    to.next_->previous_ = &to;
  }
  from.previous_ = from.next_ = nullptr;
}

AvioRenderResourceReport AllocatedImageLedger::Report(bool start_new_interval) {
  std::scoped_lock lock(mutex_);
  AvioRenderResourceReport report;
  report.available = true;
  report.counters_supported = kAvioCounterRasterThreadAllocations |
                              kAvioCounterSnapshotAllocations |
                              kAvioCounterImageUploads;
  report.raster_thread_allocations = raster_allocations_;
  report.snapshot_allocations = snapshot_allocations_;
  report.image_uploads = image_uploads_;
  for (size_t i = 0; i < usage_.size(); i++) {
    if (observed_[i]) {
      report.AddEntry(static_cast<uint32_t>(i + 1u), usage_[i]);
    }
    if (start_new_interval) {
      usage_[i].created_entries = 0u;
      usage_[i].created_real_bytes = 0u;
      usage_[i].orphans_released_entries = 0u;
      usage_[i].orphans_released_real_bytes = 0u;
    }
  }
  if (start_new_interval) {
    raster_allocations_ = snapshot_allocations_ = image_uploads_ = 0u;
  }
  return report;
}

void AllocatedImageLedger::RecordImageUpload(AvioRenderResourceKind kind,
                                             bool raster_frame) {
  if (!raster_frame || (kind != AvioRenderResourceKind::kImageTextures &&
                        kind != AvioRenderResourceKind::kGlyphAtlases)) {
    return;
  }
  std::scoped_lock lock(mutex_);
  image_uploads_++;
}

}  // namespace impeller
