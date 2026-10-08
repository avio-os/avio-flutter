// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/flow/layers/avio_hit_region_layer.h"

#include <limits>

#include "flutter/flow/compositor_context.h"
#include "flutter/flow/layers/avio_frame_metadata_layer.h"
#include "flutter/flow/layers/clip_rect_layer.h"
#include "flutter/flow/layers/layer_tree.h"
#include "flutter/flow/layers/opacity_layer.h"
#include "flutter/flow/layers/transform_layer.h"
#include "flutter/flow/testing/diff_context_test.h"
#include "flutter/flow/testing/layer_test.h"
#include "flutter/flow/testing/mock_layer.h"
#include "gtest/gtest.h"

namespace flutter::testing {
namespace {

std::shared_ptr<AvioHitRegionLayer> Claim(
    DlRect rect,
    bool enabled = true,
    AvioHitRegionKind kind = AvioHitRegionKind::kClaim,
    DlPoint offset = DlPoint()) {
  return std::make_shared<AvioHitRegionLayer>(rect, enabled, kind, offset);
}

class AvioHitRegionLayerTest : public LayerTest {
 public:
  AvioHitRegionSet regions;
  void SetUp() override {
    LayerTest::SetUp();
    preroll_context()->avio_hit_regions = &regions;
  }
};

TEST_F(AvioHitRegionLayerTest, TransformClipAndOffsetPositionTheClaim) {
  auto layer = Claim(DlRect::MakeXYWH(0, 0, 100, 60), true,
                     AvioHitRegionKind::kClaim, DlPoint(4, -10));
  auto clip = std::make_shared<ClipRectLayer>(DlRect::MakeXYWH(0, 0, 100, 40),
                                              Clip::kHardEdge);
  clip->Add(layer);
  auto opacity = std::make_shared<OpacityLayer>(128, DlPoint());
  opacity->Add(clip);
  auto transform = std::make_shared<TransformLayer>(
      DlMatrix::MakeTranslation({10.25f, 20.5f}) *
      DlMatrix::MakeScale({2.f, 1.5f, 1.f}));
  transform->Add(opacity);
  transform->Preroll(preroll_context());
  ASSERT_EQ(regions.size(), 1u);
  EXPECT_FALSE(regions.invalid());
  // Local (4, -10, 104, 50) is scaled and translated like the pixels, then
  // clipped to the clip's device rect (10.25, 20.5, 210.25, 80.5).
  EXPECT_EQ(regions[0].rect, DlRect::MakeLTRB(18.25f, 20.5f, 210.25f, 80.5f));
  EXPECT_EQ(regions[0].kind, AvioHitRegionKind::kClaim);
}

TEST_F(AvioHitRegionLayerTest, DisabledFullyClippedAndEmptyClaimNothing) {
  preroll_context()->state_stack.set_preroll_delegate(
      DlRect::MakeXYWH(0, 0, 100, 100), DlMatrix());
  Claim(DlRect::MakeXYWH(0, 0, 10, 10), false)->Preroll(preroll_context());
  Claim(DlRect::MakeXYWH(200, 0, 10, 10))->Preroll(preroll_context());
  Claim(DlRect::MakeXYWH(5, 5, 0, 10))->Preroll(preroll_context());
  EXPECT_TRUE(regions.empty());
  EXPECT_FALSE(regions.invalid());
}

TEST_F(AvioHitRegionLayerTest, ZeroOutstandingOpacityClaimsNothing) {
  for (uint8_t alpha : std::initializer_list<uint8_t>{0, 1}) {
    regions.Clear();
    auto opacity = std::make_shared<OpacityLayer>(alpha, DlPoint());
    opacity->Add(Claim(DlRect::MakeXYWH(0, 0, 10, 10)));
    opacity->Preroll(preroll_context());
    EXPECT_FALSE(regions.invalid());
    EXPECT_EQ(regions.size(), alpha == 0 ? 0u : 1u) << "alpha " << +alpha;
  }
}

TEST_F(AvioHitRegionLayerTest, AxisAlignedTransformsKeepExactBounds) {
  const auto rect = DlRect::MakeXYWH(0, 0, 40, 20);
  const DlMatrix quarter_turn = DlMatrix::MakeTranslation({100.f, 0.f}) *
                                DlMatrix::MakeRotationZ(DlDegrees(90));
  const DlMatrix mirror = DlMatrix::MakeTranslation({100.f, 0.f}) *
                          DlMatrix::MakeScale({-1.f, 1.f, 1.f});
  for (const auto& matrix : {quarter_turn, mirror}) {
    regions.Clear();
    auto transform = std::make_shared<TransformLayer>(matrix);
    transform->Add(Claim(rect));
    transform->Preroll(preroll_context());
    ASSERT_EQ(regions.size(), 1u);
    EXPECT_FALSE(regions.invalid());
    EXPECT_EQ(regions[0].rect, rect.TransformAndClipBounds(matrix));
  }
}

TEST_F(AvioHitRegionLayerTest, NonAxisAlignedTransformsFailClosed) {
  DlMatrix perspective;
  perspective.m[3] = 0.001f;
  for (const auto& matrix : {DlMatrix::MakeRotationZ(DlDegrees(30)),
                             DlMatrix::MakeSkew(0.25f, 0.f), perspective}) {
    regions.Clear();
    auto transform = std::make_shared<TransformLayer>(matrix);
    transform->Add(Claim(DlRect::MakeXYWH(0, 0, 10, 10)));
    transform->Preroll(preroll_context());
    EXPECT_TRUE(regions.invalid());
  }
  // A claim that is not collected cannot fail the frame.
  regions.Clear();
  auto rotated =
      std::make_shared<TransformLayer>(DlMatrix::MakeRotationZ(DlDegrees(30)));
  rotated->Add(Claim(DlRect::MakeXYWH(0, 0, 10, 10), false));
  rotated->Preroll(preroll_context());
  EXPECT_FALSE(regions.invalid());
  EXPECT_TRUE(regions.empty());
}

TEST_F(AvioHitRegionLayerTest, MalformedAuthoringFailsClosedEvenWhenDisabled) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  const std::shared_ptr<AvioHitRegionLayer> malformed[] = {
      Claim(DlRect::MakeLTRB(0, 0, nan, 10)),
      Claim(DlRect::MakeLTRB(10, 0, 0, 10)),
      Claim(DlRect::MakeXYWH(0, 0, 10, 10), false),
      Claim(DlRect::MakeXYWH(0, 0, 10, 10), true,
            static_cast<AvioHitRegionKind>(7)),
      Claim(DlRect::MakeXYWH(0, 0, 10, 10), true, AvioHitRegionKind::kClaim,
            DlPoint(inf, 0)),
  };
  for (size_t i = 0; i < std::size(malformed); ++i) {
    regions.Clear();
    malformed[i]->Preroll(preroll_context());
    // Index 2 is the well-formed disabled control.
    EXPECT_EQ(regions.invalid(), i != 2) << "case " << i;
    EXPECT_TRUE(regions.empty()) << "case " << i;
  }
}

TEST_F(AvioHitRegionLayerTest, SixtyFifthCollectedClaimInvalidatesTheSet) {
  preroll_context()->state_stack.set_preroll_delegate(
      DlRect::MakeXYWH(0, 0, 1000, 100), DlMatrix());
  auto root = std::make_shared<ContainerLayer>();
  // Claims that collect nothing do not consume slots.
  root->Add(Claim(DlRect::MakeXYWH(0, 500, 10, 10)));
  root->Add(Claim(DlRect::MakeXYWH(0, 0, 10, 10), false));
  for (size_t i = 0; i < kMaxAvioHitRegionsPerFrame; ++i) {
    root->Add(Claim(DlRect::MakeXYWH(i * 10, 0, 8, 8)));
  }
  root->Preroll(preroll_context());
  EXPECT_EQ(regions.size(), kMaxAvioHitRegionsPerFrame);
  EXPECT_FALSE(regions.invalid());
  Claim(DlRect::MakeXYWH(0, 20, 8, 8))->Preroll(preroll_context());
  EXPECT_EQ(regions.size(), kMaxAvioHitRegionsPerFrame);
  EXPECT_TRUE(regions.invalid());
}

TEST_F(AvioHitRegionLayerTest, RootAndNestedClaimsCollectInPreOrder) {
  auto outer = Claim(DlRect::MakeXYWH(0, 0, 200, 100), true,
                     AvioHitRegionKind::kOutputCapture);
  auto group = std::make_shared<ContainerLayer>();
  auto inner = Claim(DlRect::MakeXYWH(5, 5, 10, 10), true,
                     AvioHitRegionKind::kClaim, DlPoint(20, 30));
  auto nested = Claim(DlRect::MakeXYWH(1, 2, 3, 4));
  inner->Add(nested);
  group->Add(inner);
  outer->Add(group);
  outer->Add(Claim(DlRect::MakeXYWH(50, 50, 5, 5)));
  EXPECT_TRUE(outer->subtree_has_avio_hit_region());
  EXPECT_TRUE(group->subtree_has_avio_hit_region());
  outer->Preroll(preroll_context());
  ASSERT_EQ(regions.size(), 4u);
  EXPECT_EQ(regions[0].rect, DlRect::MakeXYWH(0, 0, 200, 100));
  EXPECT_EQ(regions[0].kind, AvioHitRegionKind::kOutputCapture);
  EXPECT_EQ(regions[1].rect, DlRect::MakeXYWH(25, 35, 10, 10));
  // The nested claim inherits the inner offset.
  EXPECT_EQ(regions[2].rect, DlRect::MakeXYWH(21, 32, 3, 4));
  EXPECT_EQ(regions[3].rect, DlRect::MakeXYWH(50, 50, 5, 5));
}

TEST_F(AvioHitRegionLayerTest, ClaimAddsNoPaintBoundsAndPassesChildFlags) {
  auto bare = Claim(DlRect::MakeXYWH(0, 0, 50, 50));
  bare->Preroll(preroll_context());
  EXPECT_TRUE(bare->paint_bounds().IsEmpty());

  const auto path = DlPath::MakeRect(DlRect::MakeXYWH(0, 0, 10, 10));
  auto layer = Claim(DlRect::MakeXYWH(0, 0, 50, 50), true,
                     AvioHitRegionKind::kClaim, DlPoint(2, 3));
  layer->Add(MockLayer::MakeOpacityCompatible(path));
  auto opacity = std::make_shared<OpacityLayer>(128, DlPoint());
  opacity->Add(layer);
  opacity->Preroll(preroll_context());
  EXPECT_EQ(layer->paint_bounds(), DlRect::MakeXYWH(2, 3, 10, 10));
  // The opacity reaches the child itself: no protective saveLayer.
  EXPECT_TRUE(opacity->children_can_accept_opacity());
}

TEST_F(AvioHitRegionLayerTest, PaintsChildrenTranslatedAndUnchanged) {
  const auto path = DlPath::MakeRect(DlRect::MakeXYWH(0, 0, 10, 10));
  const DlPaint paint(DlColor::kRed());
  auto layer = Claim(DlRect::MakeXYWH(0, 0, 50, 50), false,
                     AvioHitRegionKind::kClaim, DlPoint(2, 3));
  layer->Add(std::make_shared<MockLayer>(path, paint));
  layer->Preroll(preroll_context());
  layer->Paint(display_list_paint_context());
  DisplayListBuilder expected;
  expected.Save();
  expected.Translate(2, 3);
  expected.DrawPath(path, paint);
  expected.Restore();
  EXPECT_TRUE(DisplayListsEQ_Verbose(display_list(), expected.Build()));
}

TEST_F(AvioHitRegionLayerTest, ClaimInRootPrefixKeepsRootFactsValid) {
  AvioFrameFacts facts;
  facts.item_effect_declaration_id = 1;
  facts.item_opacity = 0.5;
  auto effect = std::make_shared<AvioFrameMetadataLayer>(facts, DlPoint());
  effect->Add(std::make_shared<MockLayer>(DlPath()));
  auto claim = Claim(DlRect::MakeXYWH(0, 0, 10, 10));
  claim->Add(effect);
  auto root = std::make_shared<ContainerLayer>();
  root->Add(claim);
  const auto collected = CollectAvioRootFrameFacts(*root);
  EXPECT_TRUE(collected.IsValid());
  EXPECT_EQ(collected.item_opacity, 0.5);
}

class AvioHitRegionLayerTreeTest : public LayerTest {
 public:
  // Prerolls |tree| the way the rasterizer does, without a GPU context.
  static void Preroll(LayerTree& tree, DlRect cull, bool collect_frame_facts) {
    DisplayListBuilder canvas(DisplayListBuilder::kMaxCullRect);
    CompositorContext compositor;
    auto frame = compositor.AcquireFrame(nullptr, &canvas, nullptr, DlMatrix(),
                                         false, true, nullptr, nullptr);
    ASSERT_NE(frame, nullptr);
    tree.Preroll(*frame, true, cull, collect_frame_facts);
  }
};

TEST_F(AvioHitRegionLayerTreeTest, OnlyTheFactsPrerollClearsAndCollects) {
  auto root = std::make_shared<ContainerLayer>();
  root->Add(Claim(DlRect::MakeXYWH(150, 10, 20, 20)));
  LayerTree tree(root, DlISize(200, 200));
  const auto full = DlRect::MakeWH(200, 200);
  Preroll(tree, full, false);
  EXPECT_TRUE(tree.avio_hit_regions().empty());

  Preroll(tree, full, true);
  ASSERT_EQ(tree.avio_hit_regions().size(), 1u);
  const AvioHitRegionSet collected = tree.avio_hit_regions();
  // A raster preroll whose damage cull misses the claim leaves the set exact.
  Preroll(tree, DlRect::MakeXYWH(0, 0, 10, 10), false);
  EXPECT_TRUE(tree.avio_hit_regions() == collected);
  // A facts preroll with a partial cull still sees the whole frame.
  Preroll(tree, DlRect::MakeXYWH(0, 0, 10, 10), true);
  EXPECT_TRUE(tree.avio_hit_regions() == collected);
  EXPECT_EQ(tree.avio_hit_regions()[0].rect, DlRect::MakeXYWH(150, 10, 20, 20));
}

TEST_F(AvioHitRegionLayerTreeTest, EachFactsPrerollStartsFromAnEmptySet) {
  auto root = std::make_shared<ContainerLayer>();
  root->Add(Claim(DlRect::MakeXYWH(0, 0, 20, 20)));
  LayerTree tree(root, DlISize(200, 200));
  // A retried frame prerolls its facts again; claims never accumulate.
  Preroll(tree, DlRect::MakeWH(200, 200), true);
  Preroll(tree, DlRect::MakeWH(200, 200), true);
  EXPECT_EQ(tree.avio_hit_regions().size(), 1u);

  auto rotated =
      std::make_shared<TransformLayer>(DlMatrix::MakeRotationZ(DlDegrees(30)));
  rotated->Add(Claim(DlRect::MakeXYWH(0, 0, 20, 20)));
  LayerTree invalid(rotated, DlISize(200, 200));
  Preroll(invalid, DlRect::MakeWH(200, 200), true);
  EXPECT_TRUE(invalid.avio_hit_regions().invalid());

  // A tree without any claim reports an empty, valid set.
  LayerTree none(std::make_shared<ContainerLayer>(), DlISize(200, 200));
  Preroll(none, DlRect::MakeWH(200, 200), true);
  EXPECT_TRUE(none.avio_hit_regions().empty());
  EXPECT_FALSE(none.avio_hit_regions().invalid());
}

TEST_F(AvioHitRegionLayerTreeTest, RetainedClaimFollowsItsMovedAncestor) {
  auto retained = Claim(DlRect::MakeXYWH(0, 0, 30, 10));
  for (int i = 0; i < 8; ++i) {
    auto transform = std::make_shared<TransformLayer>(
        DlMatrix::MakeTranslation({i * 12.5f, 40.f}));
    transform->Add(retained);
    auto root = std::make_shared<ContainerLayer>();
    root->Add(transform);
    LayerTree tree(root, DlISize(200, 200));
    Preroll(tree, DlRect::MakeWH(200, 200), true);
    ASSERT_EQ(tree.avio_hit_regions().size(), 1u);
    EXPECT_EQ(tree.avio_hit_regions()[0].rect,
              DlRect::MakeXYWH(i * 12.5f, 40, 30, 10));
  }
}

class AvioHitRegionDiffTest : public DiffContextTest {};

TEST_F(AvioHitRegionDiffTest, ClaimChangesNeverDamageButOffsetMovesPixels) {
  auto picture =
      CreateDisplayListLayer(CreateDisplayList(DlRect::MakeXYWH(0, 0, 50, 50)));
  auto first = Claim(DlRect::MakeXYWH(0, 0, 50, 50), true,
                     AvioHitRegionKind::kClaim, DlPoint(10, 10));
  first->Add(picture);
  MockLayerTree t1;
  t1.root()->Add(first);
  DiffLayerTree(t1, MockLayerTree());

  // Rect, enabled and kind all change; the pixels do not.
  auto second = Claim(DlRect::MakeXYWH(5, 5, 20, 20), false,
                      AvioHitRegionKind::kOutputCapture, DlPoint(10, 10));
  second->AssignOldLayer(first.get());
  second->Add(picture);
  MockLayerTree t2;
  t2.root()->Add(second);
  auto damage = DiffLayerTree(t2, t1);
  EXPECT_TRUE(damage.frame_damage.bounds().IsEmpty());

  auto third = Claim(DlRect::MakeXYWH(5, 5, 20, 20), false,
                     AvioHitRegionKind::kOutputCapture, DlPoint(30, 10));
  third->AssignOldLayer(second.get());
  third->Add(picture);
  MockLayerTree t3;
  t3.root()->Add(third);
  damage = DiffLayerTree(t3, t2);
  EXPECT_EQ(damage.frame_damage.bounds(), DlIRect::MakeLTRB(10, 10, 80, 60));
}

}  // namespace
}  // namespace flutter::testing
