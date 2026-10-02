// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_COVERAGE_MASK_CACHE_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_COVERAGE_MASK_CACHE_H_

#include <array>
#include <memory>
#include <optional>

#include "impeller/entity/avio_region_packing.h"
#include "impeller/entity/geometry/coverage_geometry.h"

namespace impeller {

// Raster-thread bookkeeping shared by the native mask cache and CPU tests.
// Path is immutable and has exact equality; Lease::IsValid proves the original
// physical allocation still exists. No insertion allocates cache storage.
template <class Path, class Lease>
class CoverageMaskCache4 {
 public:
  std::shared_ptr<Lease> Find(const Path& path,
                              uint32_t generation,
                              const CoverageRasterKey4& raster_key) {
    for (auto& entry : entries_) {
      if (entry && !entry->lease->IsValid()) {
        entry.reset();
      }
      if (entry && entry->generation == generation && entry->path == path &&
          entry->raster_key == raster_key) {
        return entry->lease;
      }
    }
    return nullptr;
  }

  bool CanInsert() const {
    for (const auto& entry : entries_) {
      if (!entry || !entry->lease->IsValid()) {
        return true;
      }
    }
    return false;
  }

  bool Insert(const Path& path,
              uint32_t generation,
              const CoverageRasterKey4& raster_key,
              std::shared_ptr<Lease> lease) {
    if (!lease || !lease->IsValid()) {
      return false;
    }
    for (auto& entry : entries_) {
      if (!entry || !entry->lease->IsValid()) {
        entry.emplace(Entry{path, generation, raster_key, std::move(lease)});
        return true;
      }
    }
    return false;
  }

  void Clear() {
    for (auto& entry : entries_) {
      // Do not call Lease::Release. Other readers own shared references and
      // their original allocation remains immutable until the final drop.
      entry.reset();
    }
  }

 private:
  struct Entry {
    Path path;
    uint32_t generation;
    CoverageRasterKey4 raster_key;
    std::shared_ptr<Lease> lease;
  };
  // This is the actual region allocator's bookkeeping limit. A separate
  // arbitrary texture budget would break fixed-region admission accounting.
  std::array<std::optional<Entry>, AvioRegionPacking::kMaximumAllocations>
      entries_;
};

}  // namespace impeller
#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_COVERAGE_MASK_CACHE_H_
