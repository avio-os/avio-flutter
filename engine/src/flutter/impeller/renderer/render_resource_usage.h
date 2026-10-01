// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_RENDER_RESOURCE_USAGE_H_
#define FLUTTER_IMPELLER_RENDERER_RENDER_RESOURCE_USAGE_H_

#include <chrono>
#include <cstddef>

namespace impeller {

/// Accounted resources held by a context-owned idle cache.
struct ResourceCacheUsage {
  size_t entries = 0u;
  size_t bytes = 0u;

  constexpr bool operator==(const ResourceCacheUsage&) const = default;
};

/// Exact before/after accounting for an idle-only cache trim or release.
struct ResourceCacheTrimResult {
  ResourceCacheUsage before;
  ResourceCacheUsage after;
  /// Entries kept because a render target or submitted GPU work still
  /// references them.
  size_t kept_in_use = 0u;
  /// Idle entries kept because they were used within the recency window.
  size_t kept_recent = 0u;

  constexpr bool operator==(const ResourceCacheTrimResult&) const = default;
};

/// Exact usage report of an engine-private render-resource cache (the Vulkan
/// transient attachment pool or the offscreen RenderTargetCache).
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

/// The recency below which a cache entry is not idle. An idle release keeps
/// any entry used within this window, so entries leased by frames that do not
/// present, and the warm set of a view that rendered moments ago, survive a
/// release request. The engine owns this window; a host only asks for a
/// release on a fact it owns.
inline constexpr std::chrono::seconds kIdleReleaseMinUnused{5};

/// A request to free idle entries of engine-private render-resource caches.
struct IdleResourceRelease {
  /// Entries used more recently than this are kept.
  std::chrono::nanoseconds unused_for = kIdleReleaseMinUnused;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_RENDER_RESOURCE_USAGE_H_
