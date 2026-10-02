// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
import 'dart:ui' as ui;

import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('root alpha reuses its retained layer and validates every update', () {
    final RenderAvioItemEffect root = RenderAvioItemEffect(opacity: 0.25);
    final AvioItemEffectLayer layer =
        root.updateCompositedLayer(oldLayer: null) as AvioItemEffectLayer;
    root.opacity = 0;
    expect(root.updateCompositedLayer(oldLayer: layer), same(layer));
    expect(layer.opacity, 0);
    root.opacity = null;
    expect(root.updateCompositedLayer(oldLayer: layer), same(layer));
    expect(layer.opacity, isNull);
    root.declarationId = 17;
    root.opacity = 1;
    expect(root.updateCompositedLayer(oldLayer: layer), same(layer));
    expect(layer.opacity, 1);
    expect(layer.declarationId, 17);
    expect(() => root.declarationId = 0, throwsRangeError);
    for (final double invalid in <double>[-0.01, 1.01, double.nan, double.infinity]) {
      expect(() => root.opacity = invalid, throwsRangeError);
      expect(() => AvioItemEffectLayer(opacity: invalid), throwsRangeError);
    }
    root.dispose();
  });

  test('ready content changes preserve kind and framework layer identity', () {
    final RenderAvioReadyContent root = RenderAvioReadyContent(contentRevision: null);
    final AvioReadyContentLayer layer =
        root.updateCompositedLayer(oldLayer: null) as AvioReadyContentLayer;
    expect(layer.contentRevision, isNull);
    root.contentRevision = 42;
    root.kind = ui.AvioReadyContentKind.live;
    expect(root.updateCompositedLayer(oldLayer: layer), same(layer));
    expect(layer.contentRevision, 42);
    expect(layer.kind, ui.AvioReadyContentKind.live);
    root.contentRevision = null;
    expect(root.updateCompositedLayer(oldLayer: layer), same(layer));
    expect(layer.contentRevision, isNull);
    expect(() => root.contentRevision = 0, throwsRangeError);
    root.dispose();
  });

  test('split ground copies four root logical regions and preserves the layer', () {
    final List<ui.AvioOutputGroundRegion> input = <ui.AvioOutputGroundRegion>[
      for (int i = 0; i < 4; ++i)
        ui.AvioOutputGroundRegion(
          rect: ui.Rect.fromLTWH(i * 100, 0, 100, 200),
          color: ui.Color(0xff123456 + i),
        ),
    ];
    final RenderAvioOutputGround root = RenderAvioOutputGround(regions: input);
    final AvioOutputGroundLayer layer =
        root.updateCompositedLayer(oldLayer: null) as AvioOutputGroundLayer;
    input.clear();
    expect(layer.regions, hasLength(4));
    expect(layer.regions[3].rect, const ui.Rect.fromLTWH(300, 0, 100, 200));
    root.regions = const <ui.AvioOutputGroundRegion>[];
    expect(root.updateCompositedLayer(oldLayer: layer), same(layer));
    expect(layer.color, isNull);
    expect(layer.regions, isEmpty); // Authored clear remains a root fact.
    expect(
      () => root.regions = List<ui.AvioOutputGroundRegion>.filled(
        5,
        const ui.AvioOutputGroundRegion(rect: ui.Rect.fromLTWH(0, 0, 1, 1), color: ui.Color(0)),
      ),
      throwsArgumentError,
    );
    root.dispose();
  });
}
