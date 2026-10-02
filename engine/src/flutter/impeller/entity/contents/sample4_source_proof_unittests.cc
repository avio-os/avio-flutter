// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/core/sample4_source_proof.h"

#include "gtest/gtest.h"

namespace impeller {
namespace {

TEST(Sample4SourceProofTest, TokenAndBothSourcePredicatesAreRequired) {
  AvioSample4SourceProof proof;
  EXPECT_FALSE(proof.Matches(1));
  proof.segment_token = 1;
  proof.uniform_samples = true;
  EXPECT_FALSE(proof.Matches(1));
  proof.full_clip_geometry = true;
  EXPECT_TRUE(proof.Matches(1));
  EXPECT_FALSE(proof.Matches(0));
  EXPECT_FALSE(proof.Matches(2));
  proof.uniform_samples = false;
  EXPECT_FALSE(proof.Matches(1));
}

TEST(Sample4SourceProofTest, CapturedOpaqueFirstSourceClosesPainterRounding) {
  AvioSample4OpaqueGroupProof group;
  EXPECT_FALSE(group.IsValid());
  group.Add(true, true);
  ASSERT_TRUE(group.IsValid());
  // Partial-alpha uniform paint is safe after the captured opaque overwrite.
  group.Add(true, false);
  EXPECT_TRUE(group.IsValid());
  group.Add(false, true);
  EXPECT_FALSE(group.IsValid());
  group.Add(true, true);
  EXPECT_FALSE(group.IsValid());
}

TEST(Sample4SourceProofTest, TranslucentOrDestructivePrefixDoesNotCertify) {
  AvioSample4OpaqueGroupProof ordinary;
  // The consumer already requires each original source to be full/uniform.
  ordinary.Add(true, true);
  EXPECT_TRUE(ordinary.IsValid());
  AvioSample4OpaqueGroupProof translucent;
  translucent.Add(true, false);
  translucent.Add(true, true);
  EXPECT_FALSE(translucent.IsValid());
  AvioSample4OpaqueGroupProof destructive;
  destructive.Add(false, true);
  EXPECT_FALSE(destructive.IsValid());
}

}  // namespace
}  // namespace impeller
