// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/vulkan/swapchain/transients_pool_vk.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <utility>

#include "flutter/fml/logging.h"

namespace impeller {

size_t TransientsPoolVK::ResolveByteBudgetFromEnv(size_t default_bytes) {
  const char* override_value = std::getenv(kBudgetEnvVar);
  if (override_value == nullptr || override_value[0] == '\0') {
    return default_bytes;
  }
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(override_value, &end, 10);
  constexpr size_t kBytesPerMiB = 1024ull * 1024ull;
  if (end == override_value || *end != '\0' || parsed == 0u ||
      parsed > std::numeric_limits<size_t>::max() / kBytesPerMiB) {
    FML_LOG(WARNING) << kBudgetEnvVar
                     << " set to invalid value, using default budget: "
                     << override_value;
    return default_bytes;
  }
  return static_cast<size_t>(parsed) * kBytesPerMiB;
}

TransientsPoolVK::TransientsPoolVK(std::weak_ptr<Context> context,
                                   PixelFormat depth_stencil_format,
                                   bool supports_memoryless_textures,
                                   TransientsPoolLimitsVK limits)
    : context_(std::move(context)),
      depth_stencil_format_(depth_stencil_format),
      supports_memoryless_textures_(supports_memoryless_textures),
      max_entries_(limits.max_entries),
      max_bytes_(limits.allow_environment_override
                     ? ResolveByteBudgetFromEnv(limits.max_bytes)
                     : limits.max_bytes) {}

TransientsPoolVK::~TransientsPoolVK() {
  Reset();
}

void TransientsPoolVK::Reset() {
  std::scoped_lock lock(mutex_);
  lru_.clear();
  total_bytes_ = 0;
  owner_keys_.clear();
}

TransientsPoolVK::Key TransientsPoolVK::KeyFor(const TextureDescriptor& desc,
                                               bool enable_msaa) {
  return Key{
      .width = static_cast<int>(desc.size.width),
      .height = static_cast<int>(desc.size.height),
      .color_format = desc.format,
      .enable_msaa = enable_msaa,
  };
}

std::shared_ptr<SwapchainTransientsVK> TransientsPoolVK::Acquire(
    const TextureDescriptor& desc,
    bool enable_msaa,
    TransientsPoolRefusalVK* refusal) {
  std::scoped_lock lock(mutex_);
  const auto entry = AcquireLocked(desc, enable_msaa, refusal);
  return entry == lru_.end() ? nullptr : entry->transients;
}

std::shared_ptr<SwapchainTransientsVK> TransientsPoolVK::Acquire(
    const TextureDescriptor& desc,
    bool enable_msaa,
    TransientsOwnerVK owner,
    TransientsPoolRefusalVK* refusal) {
  std::scoped_lock lock(mutex_);
  // The owner's claim moves to the key it acquires now. Withdraw it before
  // admission so that, if the caps force an eviction, the extent this owner
  // is leaving counts as an orphan (when no other owner holds it) and goes
  // before another view's warm set.
  std::optional<Key> previous;
  if (const auto held = owner_keys_.find(owner.view_id);
      held != owner_keys_.end()) {
    previous = held->second;
    owner_keys_.erase(held);
  }
  const auto entry = AcquireLocked(desc, enable_msaa, refusal);
  if (entry == lru_.end()) {
    // Refused: the owner keeps the extent it already holds.
    if (previous.has_value()) {
      owner_keys_.insert_or_assign(owner.view_id, *previous);
    }
    return nullptr;
  }
  // The owner now holds this key. A key it held before has lost the owner;
  // if no other owner holds it, it is an orphan that a later ReleaseOwner or
  // ReleaseOrphans frees once idle. Nothing is freed here except by the caps.
  entry->owned = true;
  owner_keys_.insert_or_assign(owner.view_id, entry->key);
  return entry->transients;
}

std::list<TransientsPoolVK::Entry>::iterator TransientsPoolVK::AcquireLocked(
    const TextureDescriptor& desc,
    bool enable_msaa,
    TransientsPoolRefusalVK* refusal) {
  if (refusal) {
    *refusal = {};
  }
  const Key key = KeyFor(desc, enable_msaa);

  // Hit: the key's one entry, whatever its lease. A leased entry may still be
  // referenced by a pending render target or by in-flight GPU work; sharing
  // it is safe because every user records on the raster thread and submits to
  // the context's one graphics queue, where each render pass clears both
  // attachments behind an incoming dependency that orders it after earlier
  // passes' writes (see SwapchainTransientsVK).
  if (const auto found = FindLocked(key); found != lru_.end()) {
    lru_.splice(lru_.begin(), lru_, found);
    SampleLeasedLocked(lru_.begin());
    return lru_.begin();
  }

  // Miss: construct the key's one entry. Bind the transients to the same
  // context we hold weakly so its lifetime cannot outlast the owning
  // ContextVK.
  const auto footprint = ComputeFootprint(desc, enable_msaa);
  if (!footprint.has_value() || !ReserveFor(*footprint)) {
    if (refusal) {
      *refusal = {
          .refused = true,
          .invalid_footprint = !footprint.has_value(),
          .entry_limit = lru_.size() >= max_entries_,
          .byte_limit =
              footprint.has_value() && (*footprint > max_bytes_ ||
                                        total_bytes_ > max_bytes_ - *footprint),
          .entries = lru_.size(),
          .bytes = total_bytes_,
          .requested_bytes = footprint.value_or(0u),
      };
    }
    return lru_.end();
  }
  // ReserveFor only removes entries, so the key is still absent.
  FML_DCHECK(FindLocked(key) == lru_.end());
  lru_.push_front(Entry{
      .key = key,
      .transients =
          std::make_shared<SwapchainTransientsVK>(context_, desc, enable_msaa),
      .byte_footprint = *footprint,
      .created_interval = interval_,
  });
  total_bytes_ += *footprint;
  created_entries_++;
  SampleLeasedLocked(lru_.begin());
  return lru_.begin();
}

void TransientsPoolVK::SampleLeasedLocked(
    std::list<Entry>::const_iterator acquired) {
  // The caller is about to hold `acquired`, so it counts as leased.
  size_t leased = 0u;
  for (auto it = lru_.cbegin(); it != lru_.cend(); ++it) {
    if (it == acquired || !EntryIsIdle(*it)) {
      leased += it->byte_footprint;
    }
  }
  peak_leased_nominal_bytes_ = std::max(peak_leased_nominal_bytes_, leased);
}

std::list<TransientsPoolVK::Entry>::iterator TransientsPoolVK::EraseLocked(
    std::list<Entry>::iterator it) {
  const size_t real_bytes = it->transients->GetAllocatedByteSize();
  if (EntryIsOrphanLocked(*it)) {
    orphans_released_entries_++;
    orphans_released_real_bytes_ += real_bytes;
  }
  if (it->created_interval == interval_) {
    created_erased_real_bytes_ += real_bytes;
  }
  total_bytes_ -= it->byte_footprint;
  // Dropping the pool's reference hands the textures to the context's
  // resource manager, which destroys them off this thread.
  return lru_.erase(it);
}

std::optional<size_t> TransientsPoolVK::ComputeFootprint(
    const TextureDescriptor& desc,
    bool enable_msaa) const {
  if (supports_memoryless_textures_) {
    // Lazily-allocated attachments do not occupy persistent VRAM. They
    // still count against the entry-count cap so the cache cannot grow
    // unbounded with a churning set of distinct view sizes.
    return 0u;
  }
  if (desc.size.width <= 0 || desc.size.height <= 0) {
    return std::nullopt;
  }
  const size_t width = static_cast<size_t>(desc.size.width);
  const size_t height = static_cast<size_t>(desc.size.height);
  if (width > std::numeric_limits<size_t>::max() / height) {
    return std::nullopt;
  }
  const size_t pixels = width * height;
  const size_t color_samples = enable_msaa
                                   ? static_cast<size_t>(SampleCount::kCount4)
                                   : static_cast<size_t>(SampleCount::kCount1);
  const size_t depth_samples = enable_msaa
                                   ? static_cast<size_t>(SampleCount::kCount4)
                                   : static_cast<size_t>(SampleCount::kCount1);
  // MSAA-only color allocation: if MSAA is disabled the resolve target is
  // the swapchain image itself (owned externally), so the pool holds no
  // color attachment in that case.
  const auto attachment_bytes = [pixels](
                                    size_t bytes_per_pixel,
                                    size_t samples) -> std::optional<size_t> {
    if (bytes_per_pixel == 0u || samples == 0u ||
        pixels > std::numeric_limits<size_t>::max() / bytes_per_pixel) {
      return std::nullopt;
    }
    const size_t single_sample_bytes = pixels * bytes_per_pixel;
    if (single_sample_bytes > std::numeric_limits<size_t>::max() / samples) {
      return std::nullopt;
    }
    return single_sample_bytes * samples;
  };
  const auto color_bytes =
      enable_msaa ? attachment_bytes(BytesPerPixelForPixelFormat(desc.format),
                                     color_samples)
                  : std::optional<size_t>{0u};
  const auto depth_bytes = attachment_bytes(
      BytesPerPixelForPixelFormat(depth_stencil_format_), depth_samples);
  if (!color_bytes.has_value() || !depth_bytes.has_value() ||
      *color_bytes > std::numeric_limits<size_t>::max() - *depth_bytes) {
    return std::nullopt;
  }
  return *color_bytes + *depth_bytes;
}

bool TransientsPoolVK::ReserveFor(size_t byte_footprint) {
  if (max_entries_ == 0u || byte_footprint > max_bytes_) {
    return false;
  }
  while (lru_.size() >= max_entries_ ||
         total_bytes_ > max_bytes_ - byte_footprint) {
    // An idle orphan holds an extent no existing owner uses, so it goes
    // first; otherwise the least recently used idle entry.
    auto candidate = lru_.end();
    for (auto it = lru_.end(); it != lru_.begin();) {
      --it;
      if (EntryIsIdle(*it) && EntryIsOrphanLocked(*it)) {
        candidate = it;
        break;
      }
    }
    for (auto it = lru_.end(); candidate == lru_.end() && it != lru_.begin();) {
      --it;
      if (EntryIsIdle(*it)) {
        candidate = it;
      }
    }
    if (candidate == lru_.end()) {
      return false;
    }
    EraseLocked(candidate);
  }
  return true;
}

bool TransientsPoolVK::EntryIsIdle(const Entry& entry) const {
  return entry.transients.use_count() == 1u && entry.transients->IsIdle();
}

bool TransientsPoolVK::KeyHasOwnerLocked(const Key& key) const {
  for (const auto& [_, owned_key] : owner_keys_) {
    if (owned_key == key) {
      return true;
    }
  }
  return false;
}

bool TransientsPoolVK::EntryIsOrphanLocked(const Entry& entry) const {
  return entry.owned && !KeyHasOwnerLocked(entry.key);
}

TransientsOrphansReleasedVK TransientsPoolVK::ReleaseIdleOrphansLocked() {
  TransientsOrphansReleasedVK released;
  for (auto it = lru_.begin(); it != lru_.end();) {
    if (!EntryIsOrphanLocked(*it) || !EntryIsIdle(*it)) {
      ++it;
      continue;
    }
    released.entries++;
    released.bytes += it->byte_footprint;
    it = EraseLocked(it);
  }
  return released;
}

TransientsOrphansReleasedVK TransientsPoolVK::ReleaseOwner(
    TransientsOwnerVK owner) {
  std::scoped_lock lock(mutex_);
  owner_keys_.erase(owner.view_id);
  return ReleaseIdleOrphansLocked();
}

TransientsOrphansReleasedVK TransientsPoolVK::ReleaseOrphans() {
  std::scoped_lock lock(mutex_);
  return ReleaseIdleOrphansLocked();
}

std::list<TransientsPoolVK::Entry>::iterator TransientsPoolVK::FindLocked(
    const Key& key) {
  for (auto it = lru_.begin(); it != lru_.end(); ++it) {
    if (it->key == key) {
      return it;
    }
  }
  return lru_.end();
}

ResourceCacheTrimResult TransientsPoolVK::TrimIdle() {
  std::scoped_lock lock(mutex_);
  ResourceCacheTrimResult result{.before = GetUsageLocked()};
  for (auto it = lru_.begin(); it != lru_.end();) {
    if (!EntryIsIdle(*it)) {
      ++it;
      continue;
    }
    it = EraseLocked(it);
  }
  result.after = GetUsageLocked();
  return result;
}

RenderResourceUsage TransientsPoolVK::ReportUsage(bool start_new_interval) {
  std::scoped_lock lock(mutex_);
  RenderResourceUsage report;
  report.entries = lru_.size();
  report.nominal_bytes = total_bytes_;
  size_t leased_nominal_bytes = 0u;
  size_t created_live_real_bytes = 0u;
  for (auto it = lru_.begin(); it != lru_.end(); ++it) {
    const size_t real_bytes = it->transients->GetAllocatedByteSize();
    report.real_bytes += real_bytes;
    if (!EntryIsIdle(*it)) {
      report.leased_entries++;
      leased_nominal_bytes += it->byte_footprint;
    }
    if (it->created_interval == interval_) {
      created_live_real_bytes += real_bytes;
    }
    bool first_of_key = true;
    for (auto earlier = lru_.begin(); earlier != it; ++earlier) {
      if (earlier->key == it->key) {
        first_of_key = false;
        break;
      }
    }
    if (first_of_key) {
      report.distinct_keys++;
    }
  }
  report.duplicate_entries = report.entries - report.distinct_keys;
  report.peak_leased_nominal_bytes =
      std::max(peak_leased_nominal_bytes_, leased_nominal_bytes);
  report.orphans_released_entries = orphans_released_entries_;
  report.orphans_released_real_bytes = orphans_released_real_bytes_;
  report.created_entries = created_entries_;
  report.created_real_bytes =
      created_erased_real_bytes_ + created_live_real_bytes;
  if (start_new_interval) {
    interval_++;
    peak_leased_nominal_bytes_ = 0u;
    orphans_released_entries_ = 0u;
    orphans_released_real_bytes_ = 0u;
    created_entries_ = 0u;
    created_erased_real_bytes_ = 0u;
  }
  return report;
}

ResourceCacheUsage TransientsPoolVK::GetUsage() const {
  std::scoped_lock lock(mutex_);
  return GetUsageLocked();
}

ResourceCacheUsage TransientsPoolVK::GetUsageLocked() const {
  return {.entries = lru_.size(), .bytes = total_bytes_};
}

size_t TransientsPoolVK::GetEntryCountForTesting() const {
  std::scoped_lock lock(mutex_);
  return lru_.size();
}

size_t TransientsPoolVK::GetByteFootprintForTesting() const {
  std::scoped_lock lock(mutex_);
  return total_bytes_;
}

}  // namespace impeller
