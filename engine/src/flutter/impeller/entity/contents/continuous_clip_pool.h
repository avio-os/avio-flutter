// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_CONTINUOUS_CLIP_POOL_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_CONTINUOUS_CLIP_POOL_H_

#include <array>
#include <atomic>
#include <memory>

#include "impeller/core/continuous_coverage.h"

namespace impeller {

// Allocated during ContentContext initialization, before any raster turn.
// Strong expression owners pin immutable logical states across saves, clip
// replay and recorded packets. GPU uploads are copies with their own fenced
// buffer custody; the GPU never reads this CPU pool directly.
class AvioContinuousClipPool {
 public:
  static constexpr size_t kCapacity = 128;

  AvioContinuousClipPool() {
    for (auto& slot : slots_) {
      slot = std::make_shared<AvioContinuousClipExpression>();
    }
  }

  // The caller mutates the returned unshared state once, then publishes a
  // strong const owner. Admission never allocates, waits, or overwrites a live
  // logical reader. Concurrent acquisition or exhausted capacity returns null
  // and must fail the frame, rather than dropping a parent or growing storage.
  std::shared_ptr<AvioContinuousClipExpression> Acquire(
      const std::shared_ptr<const AvioContinuousClipExpression>& previous) {
    if (acquiring_.test_and_set(std::memory_order_acquire)) {
      return nullptr;
    }
    std::shared_ptr<AvioContinuousClipExpression> acquired;
    for (auto& slot : slots_) {
      if (slot.use_count() == 1) {
        // shared_ptr's last-reader decrement releases prior reader access.
        // Its count query may be relaxed; acquire before reusing CPU storage.
        // Logical readers must retain a strong owner, never resurrect a weak
        // pointer to an already returned state.
        std::atomic_thread_fence(std::memory_order_acquire);
        acquired = slot;
        *acquired = previous ? *previous : AvioContinuousClipExpression{};
        break;
      }
    }
    acquiring_.clear(std::memory_order_release);
    return acquired;
  }

  AvioContinuousClipPool(const AvioContinuousClipPool&) = delete;
  AvioContinuousClipPool& operator=(const AvioContinuousClipPool&) = delete;

 private:
  std::array<std::shared_ptr<AvioContinuousClipExpression>, kCapacity> slots_;
  std::atomic_flag acquiring_ = ATOMIC_FLAG_INIT;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_CONTINUOUS_CLIP_POOL_H_
