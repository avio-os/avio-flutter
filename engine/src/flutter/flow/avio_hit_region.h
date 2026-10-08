// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_FLOW_AVIO_HIT_REGION_H_
#define FLUTTER_FLOW_AVIO_HIT_REGION_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "flutter/display_list/geometry/dl_geometry_types.h"

namespace flutter {

constexpr size_t kMaxAvioHitRegionsPerFrame = 64;

// What an external compositor may route to this view. A claim takes pointer
// input inside its rect; an output capture asks for the whole output and is
// only meaningful on an output-sized view (the host enforces both).
enum class AvioHitRegionKind : uint8_t { kClaim = 0, kOutputCapture = 1 };

// One device-space rect of the exact scene that also carries the pixels. It is
// already positioned by every ancestor transform and clipped by every ancestor
// clip, exactly as the pixels are.
struct AvioHitRegion {
  DlRect rect;
  AvioHitRegionKind kind = AvioHitRegionKind::kClaim;
  bool operator==(const AvioHitRegion&) const = default;
};

// The complete, ordered input claim of one frame. It lives inline in the
// LayerTree so collection never allocates. An invalid set (an unsupported
// transform, a malformed rect or more than kMaxAvioHitRegionsPerFrame regions)
// fails the whole frame closed; it is never folded into a derived claim.
class AvioHitRegionSet {
 public:
  size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }
  bool invalid() const { return invalid_; }
  const AvioHitRegion& operator[](size_t i) const { return regions_[i]; }
  const AvioHitRegion* begin() const { return regions_.data(); }
  const AvioHitRegion* end() const { return regions_.data() + count_; }

  void Clear() {
    count_ = 0;
    invalid_ = false;
  }
  void Invalidate() { invalid_ = true; }
  // A region past the bound invalidates the set; earlier regions are kept so
  // diagnostics can still see what the frame authored.
  void Add(const AvioHitRegion& region) {
    if (count_ >= kMaxAvioHitRegionsPerFrame) {
      invalid_ = true;
      return;
    }
    regions_[count_++] = region;
  }

  // Order-exact: the same regions in the same paint order. Slots past size()
  // are stale storage and never compared.
  bool operator==(const AvioHitRegionSet& other) const {
    if (count_ != other.count_ || invalid_ != other.invalid_) {
      return false;
    }
    for (size_t i = 0; i < count_; ++i) {
      if (!(regions_[i] == other.regions_[i])) {
        return false;
      }
    }
    return true;
  }

 private:
  std::array<AvioHitRegion, kMaxAvioHitRegionsPerFrame> regions_{};
  uint8_t count_ = 0;
  bool invalid_ = false;
};

}  // namespace flutter

#endif  // FLUTTER_FLOW_AVIO_HIT_REGION_H_
