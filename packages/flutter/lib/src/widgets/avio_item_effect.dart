// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:ui' as ui;

import 'package:flutter/animation.dart';
import 'package:flutter/rendering.dart';

import 'framework.dart';
import 'transitions.dart';

/// View-root alpha applied externally to the matching content revision.
/// It requires the negotiated Avio item-effects engine contract and may only
/// wrap the entire item root; the engine rejects nested or sibling authors.
class AvioItemEffect extends SingleChildRenderObjectWidget {
  /// Creates a root alpha author without an engine saveLayer.
  const AvioItemEffect({
    super.key,
    required this.opacity,
    this.declarationId = 1,
    required Widget child,
  }) : assert(opacity >= 0 && opacity <= 1),
       super(child: child);

  /// Alpha of the exact content revision.
  final double opacity;

  /// Nonzero identity of this effect declaration. Host root bridges use a fresh nonce.
  final int declarationId;

  @override
  RenderAvioItemEffect createRenderObject(BuildContext context) =>
      RenderAvioItemEffect(opacity: opacity, declarationId: declarationId);

  @override
  void updateRenderObject(BuildContext context, RenderAvioItemEffect renderObject) {
    renderObject
      ..opacity = opacity
      ..declarationId = declarationId;
  }
}

/// Animates only the root's revision alpha, retaining the child's paint layer.
class AvioItemFadeTransition extends AnimatedWidget {
  /// Creates a root fade driven by [opacity].
  const AvioItemFadeTransition({super.key, required Animation<double> opacity, required this.child})
    : super(listenable: opacity);

  /// Animation of the content revision's alpha.
  Animation<double> get opacity => listenable as Animation<double>;

  /// Root contents; opacity updates do not rebuild this child.
  final Widget child;

  @override
  Widget build(BuildContext context) => AvioItemEffect(opacity: opacity.value, child: child);
}

/// Authors an output's ground in the exact content transaction, never as a
/// placement command. A null [color] explicitly clears the authored ground.
class AvioOutputGround extends SingleChildRenderObjectWidget {
  /// Creates a root-only output fill author.
  const AvioOutputGround({
    super.key,
    required this.color,
    this.regions = const <ui.AvioOutputGroundRegion>[],
    required Widget child,
  }) : super(child: child);

  /// Solid output fill, or null to clear it.
  final Color? color;

  /// At most four non-overlapping logical view-root regions with null [color].
  final List<ui.AvioOutputGroundRegion> regions;

  @override
  RenderAvioOutputGround createRenderObject(BuildContext context) =>
      RenderAvioOutputGround(color: color, regions: regions);

  @override
  void updateRenderObject(BuildContext context, RenderAvioOutputGround renderObject) {
    renderObject
      ..color = color
      ..regions = regions;
  }
}

/// Authors the identity and kind of intended ready root content. Add this
/// decorator only after the intended content is ready, never around a loading
/// placeholder. The host accepts this exact buffered revision; only static
/// content is eligible for an explicit static seal.
class AvioReadyContent extends SingleChildRenderObjectWidget {
  /// Creates a ready-content revision author.
  const AvioReadyContent({
    super.key,
    required this.contentRevision,
    this.kind = ui.AvioReadyContentKind.static,
    required Widget child,
  }) : assert(contentRevision == null || contentRevision > 0),
       super(child: child);

  /// Fresh nonzero identity of the intended ready static or live content.
  final int? contentRevision;

  /// Only static content is eligible for an explicit static seal.
  final ui.AvioReadyContentKind kind;

  @override
  RenderAvioReadyContent createRenderObject(BuildContext context) =>
      RenderAvioReadyContent(contentRevision: contentRevision, kind: kind);

  @override
  void updateRenderObject(BuildContext context, RenderAvioReadyContent renderObject) {
    renderObject
      ..contentRevision = contentRevision
      ..kind = kind;
  }
}
