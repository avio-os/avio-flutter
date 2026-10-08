// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/compositing/scene_builder.h"

#include "flutter/display_list/dl_builder.h"
#include "flutter/flow/compositor_context.h"
#include "flutter/flow/layers/layer_tree.h"
#include "flutter/fml/synchronization/waitable_event.h"
#include "flutter/lib/ui/compositing/scene.h"
#include "flutter/shell/common/shell_test.h"
#include "flutter/testing/testing.h"

// CREATE_NATIVE_ENTRY is leaky by design.
// NOLINTBEGIN(clang-analyzer-core.StackAddressEscape)
namespace flutter::testing {

TEST_F(ShellTest, AvioHitRegionSceneBuilderCollectsRetainedClaims) {
  auto finished = std::make_shared<fml::AutoResetWaitableEvent>();
  size_t validated_scenes = 0;
  bool malformed_rect_rejected = false;
  auto validate_ancestors = [](Dart_NativeArguments args) {
    intptr_t peer = 0;
    ASSERT_FALSE(Dart_IsError(
        Dart_GetNativeInstanceField(Dart_GetNativeArgument(args, 0),
                                    tonic::DartWrappable::kPeerIndex, &peer)));
    auto* builder = reinterpret_cast<SceneBuilder*>(peer);
    ASSERT_NE(builder, nullptr);
    for (const auto& ancestor : builder->layer_stack()) {
      EXPECT_TRUE(ancestor->subtree_has_avio_hit_region());
    }
  };
  auto validate_scene = [&validated_scenes](Dart_NativeArguments args) {
    intptr_t peer = 0;
    ASSERT_FALSE(Dart_IsError(
        Dart_GetNativeInstanceField(Dart_GetNativeArgument(args, 0),
                                    tonic::DartWrappable::kPeerIndex, &peer)));
    double expected_top = 0;
    ASSERT_FALSE(Dart_IsError(
        Dart_DoubleValue(Dart_GetNativeArgument(args, 1), &expected_top)));
    bool expect_claim = false;
    ASSERT_FALSE(Dart_IsError(
        Dart_BooleanValue(Dart_GetNativeArgument(args, 2), &expect_claim)));
    auto* scene = reinterpret_cast<Scene*>(peer);
    ASSERT_NE(scene, nullptr);
    auto tree = scene->takeLayerTree(200, 200);
    ASSERT_NE(tree, nullptr);
    EXPECT_TRUE(tree->root_layer()->subtree_has_avio_hit_region());
    DisplayListBuilder canvas(DisplayListBuilder::kMaxCullRect);
    CompositorContext compositor;
    auto frame = compositor.AcquireFrame(nullptr, &canvas, nullptr, DlMatrix(),
                                         false, true, nullptr, nullptr);
    ASSERT_NE(frame, nullptr);
    // A raster preroll never collects; the facts preroll sees the whole frame
    // even with a partial cull.
    tree->Preroll(*frame, true, DlRect::MakeXYWH(150, 150, 10, 10));
    EXPECT_TRUE(tree->avio_hit_regions().empty());
    tree->Preroll(*frame, true, DlRect::MakeXYWH(150, 150, 10, 10),
                  /*collect_frame_facts=*/true);
    const auto& regions = tree->avio_hit_regions();
    EXPECT_FALSE(regions.invalid());
    EXPECT_EQ(regions.size(), expect_claim ? 1u : 0u);
    if (expect_claim && regions.size() == 1u) {
      EXPECT_EQ(regions[0].rect,
                DlRect::MakeXYWH(12, static_cast<float>(expected_top), 40, 30));
      EXPECT_EQ(regions[0].kind, AvioHitRegionKind::kClaim);
    }
    validated_scenes++;
  };
  auto validate_authoring =
      [&malformed_rect_rejected](Dart_NativeArguments args) {
        ASSERT_FALSE(Dart_IsError(Dart_BooleanValue(
            Dart_GetNativeArgument(args, 0), &malformed_rect_rejected)));
      };
  auto finish = [finished](Dart_NativeArguments) { finished->Signal(); };
  AddNativeCallback("ValidateAvioHitRegionAncestors",
                    CREATE_NATIVE_ENTRY(validate_ancestors));
  AddNativeCallback("ValidateAvioHitRegionScene",
                    CREATE_NATIVE_ENTRY(validate_scene));
  AddNativeCallback("ValidateAvioHitRegionAuthoring",
                    CREATE_NATIVE_ENTRY(validate_authoring));
  AddNativeCallback("Finish", CREATE_NATIVE_ENTRY(finish));

  auto settings = CreateSettingsForFixture();
  TaskRunners runners("hit-region-test", GetCurrentTaskRunner(),
                      CreateNewThread(), CreateNewThread(), CreateNewThread());
  auto shell = CreateShell(settings, runners);
  ASSERT_TRUE(shell->IsSetup());
  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("validateAvioHitRegionSceneBuilder");
  shell->RunEngine(std::move(configuration), [](auto result) {
    EXPECT_EQ(result, Engine::RunStatus::Success);
  });
  finished->Wait();
  EXPECT_EQ(validated_scenes, 3u);
  EXPECT_TRUE(malformed_rect_rejected);
  DestroyShell(std::move(shell), runners);
}

}  // namespace flutter::testing
// NOLINTEND(clang-analyzer-core.StackAddressEscape)
