// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/continuous_coverage_pipeline.h"

#include <cstddef>

#include "gtest/gtest.h"

namespace impeller {

TEST(AvioContinuousControlTest, PreservesOriginalOperatorAndNativePhase) {
  ColorAttachmentDescriptor original;
  original.blending_enabled = true;
  original.src_color_blend_factor = BlendFactor::kOneMinusDestinationColor;
  original.dst_color_blend_factor = BlendFactor::kOne;
  original.src_alpha_blend_factor = BlendFactor::kOne;
  original.dst_alpha_blend_factor = BlendFactor::kOneMinusSourceAlpha;
  original.color_blend_op = BlendOperation::kAdd;
  original.alpha_blend_op = BlendOperation::kReverseSubtract;
  const auto control = MakeAvioContinuousControl(original, 7, {-39, 224});
  EXPECT_EQ(control.runtime[0], 7);
  EXPECT_EQ(control.runtime[1], 0);
  EXPECT_EQ(control.origin[0], -39);
  EXPECT_EQ(control.origin[1], 224);
  EXPECT_EQ(control.factors[0],
            static_cast<float>(BlendFactor::kOneMinusDestinationColor));
  EXPECT_EQ(control.factors[1], static_cast<float>(BlendFactor::kOne));
  EXPECT_EQ(control.factors[2], static_cast<float>(BlendFactor::kOne));
  EXPECT_EQ(control.factors[3],
            static_cast<float>(BlendFactor::kOneMinusSourceAlpha));
  EXPECT_EQ(control.operations[1],
            static_cast<float>(BlendOperation::kReverseSubtract));
  EXPECT_EQ(control.operations[2], 1);
}

TEST(AvioContinuousControlTest, DeferredOwnGeometryIsAnImmutableDrawCopy) {
  ColorAttachmentDescriptor original;
  original.blending_enabled = false;
  AvioContinuousPrimitive primitive;
  primitive.vectors[0][0] = 5;
  primitive.vectors[3] = {11, 13, 29, 31};
  const auto control = MakeAvioContinuousControl(original, 0, {}, &primitive);
  primitive.vectors[3][0] = 999;
  EXPECT_EQ(control.runtime[0], 0);
  EXPECT_EQ(control.runtime[1], 1);
  EXPECT_EQ(control.operations[2], 0);
  EXPECT_EQ(control.own_geometry.vectors[3][0], 11);
  EXPECT_EQ(offsetof(AvioContinuousControl, own_geometry), 64u);
  EXPECT_EQ(sizeof(AvioContinuousControl), 704u);
  const auto following = MakeAvioContinuousControl(original, 0, {});
  EXPECT_EQ(following.runtime[1], 0);
  EXPECT_EQ(following.own_geometry, AvioContinuousPrimitive{});
}

}  // namespace impeller
