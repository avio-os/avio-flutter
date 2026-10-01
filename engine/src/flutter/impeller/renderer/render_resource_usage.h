// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_RENDER_RESOURCE_USAGE_H_
#define FLUTTER_IMPELLER_RENDERER_RENDER_RESOURCE_USAGE_H_

#include <cstddef>

namespace impeller {

/// Exact usage report of an engine-private render-resource cache (the Vulkan
/// transient attachment pool or the offscreen RenderTargetCache). Reporting
/// frees, ages and leases nothing.
///
/// Nominal bytes are the cache's own accounting of texel storage; real bytes
/// are the device-memory sizes the backend allocated (0 where the backend
/// does not know them). The interval counters cover the time since the
/// previous report that started a new interval.
struct RenderResourceUsage {
  size_t entries = 0u;
  size_t nominal_bytes = 0u;
  size_t real_bytes = 0u;
  /// Entries a render target or submitted GPU work references right now.
  size_t leased_entries = 0u;
  /// Interval: peak nominal bytes of leased entries.
  size_t peak_leased_nominal_bytes = 0u;
  size_t distinct_keys = 0u;
  /// Entries beyond one per key. The transient pool must report 0.
  size_t duplicate_entries = 0u;
  /// Interval: entries freed because no existing owner held their extent.
  size_t orphans_released_entries = 0u;
  size_t orphans_released_real_bytes = 0u;
  /// Interval: entries created and the device memory they allocated.
  size_t created_entries = 0u;
  size_t created_real_bytes = 0u;

  constexpr bool operator==(const RenderResourceUsage&) const = default;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_RENDER_RESOURCE_USAGE_H_
