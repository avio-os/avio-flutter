// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/continuous_clip_pool.h"

#include <thread>

#include "gtest/gtest.h"

namespace impeller::testing {

TEST(AvioContinuousClipPoolTest, HeldPacketsKeepStatesImmutableAtCapacity) {
  AvioContinuousClipPool pool;
  std::array<std::shared_ptr<const AvioContinuousClipExpression>,
             AvioContinuousClipPool::kCapacity>
      readers;
  for (size_t i = 0; i < readers.size(); i++) {
    auto acquired = pool.Acquire(nullptr);
    ASSERT_TRUE(acquired);
    acquired->count = 1;
    acquired->primitives[0].vectors[3][0] = i;
    readers[i] = std::move(acquired);
  }
  EXPECT_FALSE(pool.Acquire(readers[0]));
  for (size_t i = 0; i < readers.size(); i++) {
    EXPECT_EQ(readers[i]->primitives[0].vectors[3][0], i);
  }
  const auto returned_address = readers[7].get();
  readers[7].reset();
  auto successor = pool.Acquire(readers[0]);
  ASSERT_TRUE(successor);
  EXPECT_EQ(successor.get(), returned_address);
  EXPECT_EQ(successor->count, 1u);
  successor->primitives[0].vectors[3][0] = 999;
  EXPECT_EQ(readers[0]->primitives[0].vectors[3][0], 0);
}

TEST(AvioContinuousClipPoolTest,
     FrameEndAndPoolDropDoNotInvalidateSavedReaders) {
  std::shared_ptr<const AvioContinuousClipExpression> packet;
  {
    AvioContinuousClipPool pool;
    auto acquired = pool.Acquire(nullptr);
    ASSERT_TRUE(acquired);
    acquired->count = 1;
    acquired->primitives[0].vectors[3][0] = 37.25f;
    packet = std::move(acquired);
    auto next_frame = pool.Acquire(packet);
    ASSERT_TRUE(next_frame);
    EXPECT_NE(packet.get(), next_frame.get());
    next_frame->primitives[0].vectors[3][0] = 99;
  }
  ASSERT_TRUE(packet);
  EXPECT_EQ(packet->count, 1u);
  EXPECT_EQ(packet->primitives[0].vectors[3][0], 37.25f);
}

TEST(AvioContinuousClipPoolTest, ConcurrentAcquisitionNeverAliasesLiveReaders) {
  AvioContinuousClipPool pool;
  std::array<std::shared_ptr<const AvioContinuousClipExpression>, 8> owners;
  std::array<std::thread, 8> threads;
  for (size_t i = 0; i < threads.size(); i++) {
    threads[i] = std::thread([&, i] {
      // Admission contention returns null immediately; the test retries on
      // these ordinary CPU threads, while production fails the frame.
      std::shared_ptr<AvioContinuousClipExpression> acquired;
      for (size_t attempt = 0; attempt < 1000 && !acquired; attempt++) {
        acquired = pool.Acquire(nullptr);
        if (!acquired) {
          std::this_thread::yield();
        }
      }
      if (acquired) {
        acquired->count = 1;
        acquired->primitives[0].vectors[3][0] = i;
        owners[i] = std::move(acquired);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  for (size_t i = 0; i < owners.size(); i++) {
    ASSERT_TRUE(owners[i]);
    EXPECT_EQ(owners[i]->primitives[0].vectors[3][0], i);
    for (size_t j = 0; j < i; j++) {
      EXPECT_NE(owners[i].get(), owners[j].get());
    }
  }
}

}  // namespace impeller::testing
