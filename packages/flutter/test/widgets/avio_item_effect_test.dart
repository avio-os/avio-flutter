// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
import 'package:flutter/rendering.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';

class _PaintCounter extends CustomPainter {
  int paints = 0;
  @override
  void paint(Canvas canvas, Size size) {
    paints++;
    canvas.drawRect(Offset.zero & size, Paint()..color = const Color(0xff123456));
  }

  @override
  bool shouldRepaint(_PaintCounter oldDelegate) => false;
}

void main() {
  testWidgets('root fade retains child painting even through zero alpha', (
    WidgetTester tester,
  ) async {
    final AnimationController alpha = AnimationController(vsync: const TestVSync(), value: 1);
    addTearDown(alpha.dispose);
    final _PaintCounter painter = _PaintCounter();
    await tester.pumpWidget(
      AvioItemFadeTransition(
        opacity: alpha,
        child: CustomPaint(painter: painter),
      ),
    );
    final RenderAvioItemEffect root = tester.renderObject(find.byType(AvioItemEffect));
    final int paints = painter.paints;
    final Layer? layer = root.debugLayer;
    expect(paints, greaterThan(0));
    for (final double value in <double>[0.5, 0, 0.25, 1]) {
      alpha.value = value;
      await tester.pump();
      expect(painter.paints, paints);
      expect(root.debugLayer, same(layer));
      expect((root.debugLayer! as AvioItemEffectLayer).opacity, value);
    }
  });
}
