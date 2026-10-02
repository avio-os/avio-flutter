// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/coverage_clip_mask_flattener.h"

#include <limits>

#include "gtest/gtest.h"

namespace impeller::testing {
namespace {
std::shared_ptr<AvioSample4ClipRecipe> Bundle(
    const std::shared_ptr<const AvioSample4ClipRecipe>& parent = nullptr) {
  auto recipe = std::make_shared<AvioSample4ClipRecipe>();
  recipe->parent = parent;
  recipe->logical_pass_size = {512, 512};
  return recipe;
}
CoverageConvexQuad4 Quad(Rect rect = Rect::MakeXYWH(.25f, .25f, 500, 500)) {
  return *CoverageConvexQuad4::Make(rect.GetPoints());
}
}  // namespace

TEST(CoverageClipMaskFlattener, FullParentsAndDifferenceOrderRetained) {
  auto parent = Bundle();
  for (size_t i = 0; i < 4; i++) {
    ASSERT_TRUE(parent->AppendQuad(Quad(), ClipOperation::kIntersect));
    parent->nodes[i].declaration_token = i + 1;
    parent->nodes[i].clip_depth = 100 - i;
  }
  auto recipe = Bundle(parent);
  ASSERT_TRUE(recipe->AppendQuad(Quad(), ClipOperation::kDifference));
  recipe->nodes[0].declaration_token = 5;
  recipe->nodes[0].clip_depth = 96;
  ASSERT_TRUE(recipe->AppendQuad(Quad(), ClipOperation::kIntersect));
  recipe->nodes[1].declaration_token = 6;
  recipe->nodes[1].clip_depth = 95;
  EXPECT_EQ(ValidateClipMaskTile(*recipe, IRect::MakeXYWH(32, 48, 252, 252),
                                 {256, 256}),
            PreparedFillMaskStatus::kPrepared);
  size_t index = 0;
  ASSERT_TRUE(recipe->ForEachNode([&](const auto& node) {
    EXPECT_EQ(node.declaration_token, index + 1);
    EXPECT_EQ(node.clip_depth, 100 - index);
    index++;
    return true;
  }));
  EXPECT_EQ(index, 6u);
  const std::array<ClipSampleMask4, 6> lanes = {15, 7, 3, 3, 1, 15};
  // An averaged half mask would lose the surviving lane identity; all six
  // parents are applied before the final two-lane geometry intersection.
  EXPECT_EQ(recipe->Combine(lanes, 3), 2);
}

TEST(CoverageClipMaskFlattener, All512ParentsSurviveTemporaryStencilReuse) {
  AvioSample4ClipPool pool;
  std::shared_ptr<const AvioSample4ClipRecipe> recipe;
  std::array<ClipSampleMask4, 512> native_lanes;
  native_lanes.fill(15);
  native_lanes[0] = 11;
  native_lanes[256] = 7;
  native_lanes[511] = 1;
  for (size_t i = 0; i < native_lanes.size(); i++) {
    auto next = pool.Acquire(recipe);
    ASSERT_TRUE(next);
    ASSERT_TRUE(next->AppendQuad(Quad(), i == 511 ? ClipOperation::kDifference
                                                  : ClipOperation::kIntersect));
    next->logical_pass_size = {512, 512};
    auto& node = next->nodes[next->count - 1];
    node.declaration_token = i + 1;
    node.clip_depth = 500000 - i;  // not representable as an 8-bit ordinal
    recipe = std::move(next);
  }
  ASSERT_EQ(recipe->GetDepth(), 512u);
  ASSERT_EQ(ValidateClipMaskTile(*recipe, IRect::MakeSize(ISize{252, 252}),
                                 {256, 256}),
            PreparedFillMaskStatus::kPrepared);
  // The actual shader operation's production CPU counterpart visits all
  // parents in order. Dropping the old, middle or newest bundle each produces
  // a different lane mask, so a last-K4 or byte-depth truncation cannot pass.
  EXPECT_EQ(recipe->Combine(native_lanes), 2);
  EXPECT_EQ(recipe->Combine(std::span(native_lanes).last(255)), std::nullopt);
  size_t visited = 0;
  ASSERT_TRUE(recipe->ForEachNode([&](const auto& node) {
    EXPECT_EQ(node.declaration_token, visited + 1);
    EXPECT_EQ(node.clip_depth, 500000 - visited);
    visited++;
    return true;
  }));
  EXPECT_EQ(visited, 512u);
}

TEST(CoverageClipMaskFlattener, UnknownParentAndExcessDepthFailClosed) {
  auto parent = Bundle();
  parent->count = 1;  // no retained native representation
  auto recipe = Bundle(parent);
  ASSERT_TRUE(recipe->AppendQuad(Quad(), ClipOperation::kIntersect));
  EXPECT_EQ(ValidateClipMaskTile(*recipe, IRect::MakeSize(ISize{252, 252}),
                                 {256, 256}),
            PreparedFillMaskStatus::kFailed);
  parent->nodes[0].quad = Quad();
  parent->logical_pass_size = {1024, 512};
  EXPECT_EQ(ValidateClipMaskTile(*recipe, IRect::MakeSize(ISize{252, 252}),
                                 {256, 256}),
            PreparedFillMaskStatus::kFailed);
  recipe->parent.reset();
  recipe->nodes[0].clip_depth = std::numeric_limits<uint32_t>::max();
  EXPECT_EQ(ValidateClipMaskTile(*recipe, IRect::MakeSize(ISize{252, 252}),
                                 {256, 256}),
            PreparedFillMaskStatus::kFailed);
  recipe->nodes[0].clip_depth = 0;
  auto previous = recipe;
  for (size_t i = 0; i < AvioSample4ClipDescriptor::kMaxBundles; i++) {
    auto next = Bundle(previous);
    ASSERT_TRUE(next->AppendQuad(Quad(), ClipOperation::kIntersect));
    previous = std::move(next);
  }
  EXPECT_EQ(ValidateClipMaskTile(*previous, IRect::MakeSize(ISize{252, 252}),
                                 {256, 256}),
            PreparedFillMaskStatus::kFailed);
}

TEST(CoverageClipMaskFlattener, PaddedEdgeKeepsOriginalRasterOrigin) {
  auto recipe = Bundle();
  ASSERT_TRUE(recipe->AppendQuad(Quad(), ClipOperation::kIntersect));
  EXPECT_EQ(ValidateClipMaskTile(*recipe, IRect::MakeXYWH(-2, -2, 252, 252),
                                 {256, 256}),
            PreparedFillMaskStatus::kPrepared);
  EXPECT_EQ(ValidateClipMaskTile(*recipe, IRect::MakeXYWH(510, 510, 252, 252),
                                 {256, 256}),
            PreparedFillMaskStatus::kEmpty);  // complete shape ends at 500.25
  EXPECT_EQ(ValidateClipMaskTile(*recipe, IRect::MakeXYWH(520, 520, 252, 252),
                                 {256, 256}),
            PreparedFillMaskStatus::kEmpty);
  EXPECT_EQ(ValidateClipMaskTile(*recipe, IRect::MakeSize(ISize{257, 16}),
                                 {256, 256}),
            PreparedFillMaskStatus::kFailed);
  recipe->logical_pass_size = {};
  EXPECT_EQ(
      ValidateClipMaskTile(*recipe, IRect::MakeSize(ISize{16, 16}), {256, 256}),
      PreparedFillMaskStatus::kFailed);
}

TEST(CoverageClipMaskFlattener, IntegerTranslationPreservesEveryNativeLane) {
  auto quad = Quad(Rect::MakeXYWH(34.25f, 47.25f, 6.5f, 8.5f));
  const Vector2 shift{-32, -48};
  auto translated = quad.Translated(shift);
  for (int y = 44; y < 59; y++) {
    for (int x = 30; x < 44; x++) {
      EXPECT_EQ(quad.GetSampleMask(Point(x, y)),
                translated.GetSampleMask(Point(x - 32, y - 48)));
    }
  }
  const ISize parent{512, 512};
  const ISize scratch{256, 256};
  const IPoint origin{32, 48};
  const auto remap = ClipMaskReplayTransform(parent, scratch, origin);
  const auto original =
      Matrix::MakeOrthographic(parent) * Point(34.25f, 47.25f);
  const auto shifted = remap * original;
  const auto expected = Matrix::MakeOrthographic(scratch) * Point(2.25f, -.75f);
  EXPECT_FLOAT_EQ(shifted.x, expected.x);
  EXPECT_FLOAT_EQ(shifted.y, expected.y);
}

TEST(CoverageClipMaskFlattener, EmptyIntersectIsNotIdentityOrDifference) {
  auto recipe = Bundle();
  recipe->nodes[0].geometry = GeometryResult{};
  recipe->nodes[0].physical_bounds = {};
  recipe->nodes[0].operation = ClipOperation::kIntersect;
  recipe->count = 1;
  EXPECT_EQ(
      ValidateClipMaskTile(*recipe, IRect::MakeSize(ISize{32, 32}), {256, 256}),
      PreparedFillMaskStatus::kEmpty);
  recipe->nodes[0].operation = ClipOperation::kDifference;
  EXPECT_EQ(
      ValidateClipMaskTile(*recipe, IRect::MakeSize(ISize{32, 32}), {256, 256}),
      PreparedFillMaskStatus::kPrepared);
  // Bounds alone cannot stand in for a missing nonempty native tessellation.
  recipe->nodes[0].physical_bounds = Rect::MakeSize(ISize{32, 32});
  EXPECT_EQ(
      ValidateClipMaskTile(*recipe, IRect::MakeSize(ISize{32, 32}), {256, 256}),
      PreparedFillMaskStatus::kFailed);
}
}  // namespace impeller::testing
