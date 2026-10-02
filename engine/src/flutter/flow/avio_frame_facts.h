// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_FLOW_AVIO_FRAME_FACTS_H_
#define FLUTTER_FLOW_AVIO_FRAME_FACTS_H_

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace flutter {

// Immutable view-root facts travel with the content revision, never through
// placement commands. An authored ground with no color explicitly clears it.
struct AvioOutputGroundRegion {
  double left = 0, top = 0, right = 0, bottom = 0;
  uint32_t color_argb = 0;
  bool operator==(const AvioOutputGroundRegion&) const = default;
};

enum class AvioReadyContentKind : uint32_t { kStatic = 0, kLive = 1 };

struct AvioFrameFacts {
  static constexpr size_t kMaxGroundRegions = 4;
  std::array<AvioOutputGroundRegion, kMaxGroundRegions> ground_regions{};
  size_t ground_regions_count = 0;
  std::optional<double> item_opacity;
  uint64_t item_effect_declaration_id = 0;
  bool ground_authored = false;
  std::optional<uint32_t> ground_color_argb;
  std::optional<uint64_t> ready_content_revision;
  AvioReadyContentKind ready_content_kind = AvioReadyContentKind::kStatic;
  bool invalid = false;

  bool IsValid() const {
    if (ground_regions_count > kMaxGroundRegions ||
        (ground_regions_count && (!ground_authored || ground_color_argb))) {
      return false;
    }
    for (size_t i = 0; i < ground_regions_count; ++i) {
      const auto& r = ground_regions[i];
      if (!std::isfinite(r.left) || !std::isfinite(r.top) ||
          !std::isfinite(r.right) || !std::isfinite(r.bottom) ||
          r.right <= r.left || r.bottom <= r.top) {
        return false;
      }
      for (size_t j = 0; j < i; ++j) {
        const auto& other = ground_regions[j];
        if (r.left < other.right && other.left < r.right &&
            r.top < other.bottom && other.top < r.bottom) {
          return false;
        }
      }
    }
    return !invalid &&
           (item_opacity ? item_effect_declaration_id != 0u
                         : item_effect_declaration_id == 0u) &&
           (!item_opacity || (std::isfinite(*item_opacity) &&
                              *item_opacity >= 0.0 && *item_opacity <= 1.0)) &&
           (ground_authored || !ground_color_argb) &&
           (!ready_content_revision ||
            (*ready_content_revision != 0u &&
             (ready_content_kind == AvioReadyContentKind::kStatic ||
              ready_content_kind == AvioReadyContentKind::kLive)));
  }
  bool IsValidForRoot(double width, double height) const {
    if (!IsValid() || !std::isfinite(width) || !std::isfinite(height) ||
        width <= 0 || height <= 0) {
      return false;
    }
    for (size_t i = 0; i < ground_regions_count; ++i) {
      const auto& r = ground_regions[i];
      if (r.left < 0 || r.top < 0 || r.right > width || r.bottom > height) {
        return false;
      }
    }
    return true;
  }
  bool HasMetadata() const {
    return item_opacity.has_value() || ground_authored ||
           ready_content_revision.has_value() || invalid;
  }
  bool operator==(const AvioFrameFacts&) const = default;
};

}  // namespace flutter

#endif  // FLUTTER_FLOW_AVIO_FRAME_FACTS_H_
