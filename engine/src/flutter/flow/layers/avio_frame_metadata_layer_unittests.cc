// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "flutter/flow/layers/avio_frame_metadata_layer.h"

#include <limits>

#include "flutter/flow/layers/clip_rect_layer.h"
#include "flutter/flow/layers/layer_tree.h"
#include "flutter/flow/layers/transform_layer.h"
#include "flutter/flow/testing/layer_test.h"
#include "flutter/flow/testing/mock_layer.h"
#include "gtest/gtest.h"

namespace flutter::testing {
namespace {
using AvioFrameMetadataLayerTest = LayerTest;

TEST_F(AvioFrameMetadataLayerTest, RootFactsFollowSoleChildRetainedPrefix) {
  AvioFrameFacts effect;
  effect.item_effect_declaration_id = 1;
  effect.item_opacity = 0.25;
  auto opacity = std::make_shared<AvioFrameMetadataLayer>(effect, DlPoint());
  AvioFrameFacts ground;
  ground.ground_authored = true;
  ground.ground_regions_count = 2;
  ground.ground_regions[0] = {0, 0, 400, 600, 0xFF123456};
  ground.ground_regions[1] = {400, 0, 800, 600, 0xFF654321};
  auto fill = std::make_shared<AvioFrameMetadataLayer>(ground, DlPoint());
  fill->Add(std::make_shared<MockLayer>(
      DlPath::MakeRect(DlRect::MakeXYWH(0, 0, 10, 10))));
  opacity->Add(fill);
  auto transform =
      std::make_shared<TransformLayer>(DlMatrix::MakeScale({2, 2, 1}));
  transform->Add(opacity);
  auto root = std::make_shared<ContainerLayer>();
  root->Add(transform);
  LayerTree tree(root, DlISize(1600, 1200));
  EXPECT_TRUE(tree.avio_frame_facts().IsValidForRoot(800, 600));
  EXPECT_EQ(tree.avio_frame_facts().item_opacity, 0.25);
  EXPECT_EQ(tree.avio_frame_facts().ground_regions_count, 2u);
  // Ground coordinates are explicit logical root coordinates, not doubled by
  // DPR.
  EXPECT_EQ(tree.avio_frame_facts().ground_regions[1].left, 400);
  auto retained_root = std::make_shared<ContainerLayer>();
  retained_root->Add(opacity);
  EXPECT_EQ(CollectAvioRootFrameFacts(*retained_root), tree.avio_frame_facts());
}

TEST_F(AvioFrameMetadataLayerTest, SiblingsNestedFiltersAndDuplicatesReject) {
  AvioFrameFacts facts;
  facts.item_effect_declaration_id = 1;
  facts.item_opacity = 0.5;
  for (size_t scenario = 0; scenario < 3; ++scenario) {
    auto effect = std::make_shared<AvioFrameMetadataLayer>(facts, DlPoint());
    effect->Add(std::make_shared<MockLayer>(DlPath()));
    auto root = std::make_shared<ContainerLayer>();
    if (scenario == 0) {
      root->Add(effect);
      root->Add(std::make_shared<MockLayer>(DlPath()));
    } else if (scenario == 1) {
      auto clip = std::make_shared<ClipRectLayer>(
          DlRect::MakeXYWH(0, 0, 10, 10), Clip::kHardEdge);
      clip->Add(effect);
      root->Add(clip);
    } else {
      auto second = std::make_shared<AvioFrameMetadataLayer>(facts, DlPoint());
      second->Add(effect);
      root->Add(second);
    }
    EXPECT_FALSE(CollectAvioRootFrameFacts(*root).IsValid());
  }
}

TEST_F(AvioFrameMetadataLayerTest,
       ReadyRevisionKindAndInvalidOffsetAreValidated) {
  AvioFrameFacts facts;
  facts.ready_content_revision = 27;
  facts.ready_content_kind = AvioReadyContentKind::kLive;
  auto root = std::make_shared<ContainerLayer>();
  auto ready = std::make_shared<AvioFrameMetadataLayer>(facts, DlPoint());
  ready->Add(std::make_shared<MockLayer>(DlPath()));
  root->Add(ready);
  EXPECT_EQ(CollectAvioRootFrameFacts(*root).ready_content_revision, 27u);
  EXPECT_EQ(CollectAvioRootFrameFacts(*root).ready_content_kind,
            AvioReadyContentKind::kLive);
  auto invalid = std::make_shared<AvioFrameMetadataLayer>(
      facts, DlPoint(std::numeric_limits<float>::infinity(), 0));
  EXPECT_FALSE(CollectAvioRootFrameFacts(*invalid).IsValid());
}

TEST_F(AvioFrameMetadataLayerTest,
       RootOpacityPaintsOriginalPixelsWithoutSaveLayer) {
  const auto path = DlPath::MakeRect(DlRect::MakeXYWH(0, 0, 10, 10));
  const DlPaint paint(DlColor::kRed());
  AvioFrameFacts facts;
  facts.item_effect_declaration_id = 1;
  facts.item_opacity =
      0;  // Child painting is retained even at identity-external alpha zero.
  auto layer = std::make_shared<AvioFrameMetadataLayer>(facts, DlPoint(2, 3));
  layer->Add(std::make_shared<MockLayer>(path, paint));
  layer->Preroll(preroll_context());
  EXPECT_EQ(layer->paint_bounds(), DlRect::MakeXYWH(2, 3, 10, 10));
  layer->Paint(display_list_paint_context());
  DisplayListBuilder expected;
  expected.Save();
  expected.Translate(2, 3);
  expected.DrawPath(path, paint);
  expected.Restore();
  EXPECT_TRUE(DisplayListsEQ_Verbose(display_list(), expected.Build()));
}

TEST_F(AvioFrameMetadataLayerTest, EmptyChildBoundsRemainEmptyAtRoot) {
  AvioFrameFacts facts;
  facts.item_effect_declaration_id = 1;
  facts.item_opacity = 0.5;
  auto root = std::make_shared<AvioFrameMetadataLayer>(facts, DlPoint());
  root->Add(std::make_shared<MockLayer>(DlPath()));
  root->Preroll(preroll_context());
  EXPECT_TRUE(root->paint_bounds().IsEmpty());
  EXPECT_TRUE(CollectAvioRootFrameFacts(*root).IsValid());
}
}  // namespace
}  // namespace flutter::testing
