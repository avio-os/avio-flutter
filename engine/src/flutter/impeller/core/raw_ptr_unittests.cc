// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/fml/logging.h"
#include "gtest/gtest.h"
#include "impeller/core/raw_ptr.h"

namespace impeller {
namespace testing {

TEST(RawPtrTest, EmptyReferenceCannotLock) {
  raw_ptr<int> reference;
  EXPECT_EQ(reference.Lock(), nullptr);
}

TEST(RawPtrTest, ReferenceDoesNotOwnItsCacheEntry) {
  auto cache_entry = std::make_shared<int>(17);
  std::weak_ptr<int> witness = cache_entry;
  raw_ptr<int> reference(cache_entry);
  cache_entry.reset();
  EXPECT_TRUE(witness.expired());
  EXPECT_EQ(reference.Lock(), nullptr);
}

TEST(RawPtrTest, LockedOwnerSurvivesCacheInvalidation) {
  auto cache_entry = std::make_shared<int>(17);
  std::weak_ptr<int> witness = cache_entry;
  raw_ptr<int> reference(cache_entry);
  auto recorded_owner = reference.Lock();
  cache_entry.reset();
  ASSERT_TRUE(recorded_owner);
  EXPECT_EQ(*recorded_owner, 17);
  EXPECT_FALSE(witness.expired());
  recorded_owner.reset();
  EXPECT_TRUE(witness.expired());
  EXPECT_EQ(reference.Lock(), nullptr);
}

TEST(RawPtrTest, CopiedReferenceLocksExactPredecessor) {
  auto predecessor = std::make_shared<int>(17);
  raw_ptr<int> reference(predecessor);
  const auto copy = reference;
  auto successor = std::make_shared<int>(29);
  reference = raw_ptr<int>(successor);
  EXPECT_EQ(copy.Lock(), predecessor);
  EXPECT_EQ(reference.Lock(), successor);
  predecessor.reset();
  EXPECT_EQ(copy.Lock(), nullptr);
  EXPECT_EQ(*reference.Lock(), 29);
}

TEST(RawPtrTest, ConstResourceRetainsItsTypedOwner) {
  std::shared_ptr<const int> cache_entry = std::make_shared<int>(17);
  const raw_ptr<const int> reference(cache_entry);
  auto owner = reference.Lock();
  cache_entry.reset();
  ASSERT_TRUE(owner);
  EXPECT_EQ(*owner, 17);
}

}  // namespace testing
}  // namespace impeller
