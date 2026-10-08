// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

part of 'ui_test.dart';

@pragma('vm:external-name', 'ValidateAvioHitRegionAncestors')
external void _validateHitRegionAncestors(SceneBuilder builder);

@pragma('vm:external-name', 'ValidateAvioHitRegionScene')
external void _validateHitRegionScene(Scene scene, double expectedTop, bool expectClaim);

@pragma('vm:external-name', 'ValidateAvioHitRegionAuthoring')
external void _validateHitRegionAuthoring(bool malformedRectRejected);

void runAvioHitRegionSceneBuilder() {
  const Rect rect = Rect.fromLTWH(0, 0, 40, 30);
  final SceneBuilder first = SceneBuilder();
  first.pushOffset(10, 20);
  final ClipRectEngineLayer retained = first.pushClipRect(const Rect.fromLTWH(0, 0, 100, 100));
  final AvioHitRegionEngineLayer claim = first.pushAvioHitRegion(
    rect: rect,
    offset: const Offset(2, 3),
  );
  _validateHitRegionAncestors(first);
  first.pop();
  first.pop();
  first.pop();
  final Scene firstScene = first.build();
  _validateHitRegionScene(firstScene, 23, true);
  firstScene.dispose();

  // Framework retention inserts the built subtree under a moved parent. The
  // claim follows the new offset in that same frame.
  final SceneBuilder second = SceneBuilder();
  second.pushOffset(10, 50);
  second.addRetained(retained);
  _validateHitRegionAncestors(second);
  second.pop();
  final Scene secondScene = second.build();
  _validateHitRegionScene(secondScene, 53, true);
  secondScene.dispose();

  // Disabling updates the same engine layer: nothing is claimed.
  final SceneBuilder third = SceneBuilder();
  third.pushOffset(10, 20);
  third.pushAvioHitRegion(rect: rect, enabled: false, offset: const Offset(2, 3), oldLayer: claim);
  third.pop();
  third.pop();
  final Scene thirdScene = third.build();
  _validateHitRegionScene(thirdScene, 0, false);
  thirdScene.dispose();
  retained.dispose();

  var malformedRectRejected = false;
  try {
    SceneBuilder().pushAvioHitRegion(rect: const Rect.fromLTRB(10, 0, 0, 10));
  } on ArgumentError {
    malformedRectRejected = true;
  }
  _validateHitRegionAuthoring(malformedRectRejected);
  _finish();
}
