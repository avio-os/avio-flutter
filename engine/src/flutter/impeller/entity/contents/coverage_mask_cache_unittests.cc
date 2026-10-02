// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/coverage_mask_cache.h"

#include "gtest/gtest.h"

namespace impeller {
namespace testing {
namespace {

struct ImmutablePath {
  uint32_t geometry;
  uint32_t fill_rule;
  bool operator==(const ImmutablePath&) const = default;
};
struct MaskLease {
  explicit MaskLease(size_t* drops = nullptr) : drops(drops) {}
  ~MaskLease() {
    if (drops) {
      (*drops)++;
    }
  }
  bool IsValid() const { return valid; }
  bool valid = true;
  size_t* drops;
};

using Cache = CoverageMaskCache4<ImmutablePath, MaskLease>;

}  // namespace

TEST(CoverageMaskCacheTest, ExactPathAndWindingVerifyGeneration) {
  Cache cache;
  auto key = CoverageRasterKey4::Make(Matrix{}, IRect::MakeLTRB(0, 0, 8, 8));
  ASSERT_TRUE(key.has_value());
  auto lease = std::make_shared<MaskLease>();
  ASSERT_TRUE(cache.Insert({7, 0}, 42, key.value(), lease));
  ASSERT_EQ(cache.Find({7, 0}, 42, key.value()), lease);
  ASSERT_FALSE(cache.Find({8, 0}, 42, key.value()));
  ASSERT_FALSE(cache.Find({7, 1}, 42, key.value()));
  ASSERT_FALSE(cache.Find({7, 0}, 43, key.value()));
  auto translated = CoverageRasterKey4::Make(
      Matrix::MakeTranslation({0.125, 0, 0}), IRect::MakeLTRB(0, 0, 8, 8));
  ASSERT_TRUE(translated.has_value());
  ASSERT_FALSE(cache.Find({7, 0}, 42, translated.value()));
}

TEST(CoverageMaskCacheTest, ClearingDoesNotReleaseRetainedReader) {
  Cache cache;
  auto key = CoverageRasterKey4::Make(Matrix{}, IRect::MakeLTRB(0, 0, 8, 8));
  ASSERT_TRUE(key.has_value());
  size_t drops = 0;
  auto reader = std::make_shared<MaskLease>(&drops);
  ASSERT_TRUE(cache.Insert({7, 0}, 42, key.value(), reader));
  cache.Clear();
  ASSERT_TRUE(reader->IsValid());
  ASSERT_EQ(drops, 0u);
  ASSERT_FALSE(cache.Find({7, 0}, 42, key.value()));
  reader.reset();
  ASSERT_EQ(drops, 1u);
}

TEST(CoverageMaskCacheTest, StaleEpochCannotHitAndSlotIsReusable) {
  Cache cache;
  auto key = CoverageRasterKey4::Make(Matrix{}, IRect::MakeLTRB(0, 0, 8, 8));
  ASSERT_TRUE(key.has_value());
  auto stale = std::make_shared<MaskLease>();
  ASSERT_TRUE(cache.Insert({7, 0}, 42, key.value(), stale));
  stale->valid = false;
  ASSERT_FALSE(cache.Find({7, 0}, 42, key.value()));
  auto replacement = std::make_shared<MaskLease>();
  ASSERT_TRUE(cache.Insert({7, 0}, 42, key.value(), replacement));
  ASSERT_EQ(cache.Find({7, 0}, 42, key.value()), replacement);
}

TEST(CoverageMaskCacheTest, AdmissionUsesActualFixedRegionLimit) {
  Cache cache;
  auto key = CoverageRasterKey4::Make(Matrix{}, IRect::MakeLTRB(0, 0, 8, 8));
  ASSERT_TRUE(key.has_value());
  for (uint32_t index = 0; index < AvioRegionPacking::kMaximumAllocations;
       index++) {
    ASSERT_TRUE(cache.Insert({index, 0}, index, key.value(),
                             std::make_shared<MaskLease>()));
  }
  ASSERT_FALSE(cache.CanInsert());
  ASSERT_FALSE(
      cache.Insert({999, 0}, 999, key.value(), std::make_shared<MaskLease>()));
  ASSERT_TRUE(cache.Find({0, 0}, 0, key.value()));
  cache.Clear();
  ASSERT_TRUE(cache.CanInsert());
}

}  // namespace testing
}  // namespace impeller
