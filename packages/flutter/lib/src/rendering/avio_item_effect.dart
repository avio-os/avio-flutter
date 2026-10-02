// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:ui' as ui;

import 'box.dart';
import 'layer.dart';
import 'object.dart';
import 'proxy_box.dart';
import 'view.dart';

bool _debugRootPosition(RenderObject object) {
  RenderObject? ancestor = object.parent;
  while (ancestor != null) {
    if (ancestor is RenderView) {
      return true;
    }
    if (ancestor is! RenderProxyBox) {
      return false;
    }
    ancestor = ancestor.parent;
  }
  return true; // A detached render object has no position to validate yet.
}

/// A retained root fact whose alpha is applied to the exact content revision
/// by the external compositor, without an opacity layer in the engine.
class AvioItemEffectLayer extends OffsetLayer {
  /// Creates view-root opacity metadata.
  AvioItemEffectLayer({double? opacity = 1, int declarationId = 1, super.offset})
    : _opacity = opacity,
      _declarationId = declarationId {
    if (declarationId <= 0) throw RangeError.value(declarationId, 'declarationId');
    if (opacity != null && (!opacity.isFinite || opacity < 0 || opacity > 1)) {
      throw RangeError.value(opacity, 'opacity');
    }
  }

  /// Alpha of this root's content revision.
  double? get opacity => _opacity;
  double? _opacity;
  set opacity(double? value) {
    if (value != null && (!value.isFinite || value < 0 || value > 1)) {
      throw RangeError.value(value, 'opacity');
    }
    if (value != _opacity) {
      _opacity = value;
      markNeedsAddToScene();
    }
  }

  /// Nonzero identity of this mounted effect declaration.
  int get declarationId => _declarationId;
  int _declarationId;
  set declarationId(int value) {
    if (value <= 0) throw RangeError.value(value, 'declarationId');
    if (_declarationId != value) {
      _declarationId = value;
      markNeedsAddToScene();
    }
  }

  @override
  void addToScene(ui.SceneBuilder builder) {
    final double? alpha = opacity;
    final ui.EngineLayer? previous = engineLayer;
    if (alpha == null) {
      engineLayer = builder.pushOffset(
        offset.dx,
        offset.dy,
        oldLayer: previous is ui.OffsetEngineLayer ? previous : null,
      );
    } else {
      engineLayer = builder.pushAvioItemEffect(
        opacity: alpha,
        declarationId: declarationId,
        offset: offset,
        oldLayer: previous is ui.AvioItemEffectEngineLayer ? previous : null,
      );
    }
    addChildrenToScene(builder);
    builder.pop();
  }
}

bool _sameGroundRegions(List<ui.AvioOutputGroundRegion> a, List<ui.AvioOutputGroundRegion> b) {
  if (identical(a, b)) return true;
  if (a.length != b.length) return false;
  for (int i = 0; i < a.length; ++i) {
    if (a[i].rect != b[i].rect || a[i].color != b[i].color) return false;
  }
  return true;
}

List<ui.AvioOutputGroundRegion> _copyGroundRegions(List<ui.AvioOutputGroundRegion> regions) {
  if (regions.length > 4 ||
      regions.any((ui.AvioOutputGroundRegion r) => !r.rect.isFinite || r.rect.isEmpty)) {
    throw ArgumentError('Ground must contain at most four valid root regions.');
  }
  return List<ui.AvioOutputGroundRegion>.unmodifiable(regions);
}

/// Retained root output-ground author; null explicitly clears the fill.
class AvioOutputGroundLayer extends OffsetLayer {
  /// Creates an author for this output's fill.
  AvioOutputGroundLayer({
    ui.Color? color,
    List<ui.AvioOutputGroundRegion> regions = const <ui.AvioOutputGroundRegion>[],
    super.offset,
  }) : _color = color,
       _regions = _copyGroundRegions(regions);

  /// Fill color, or null to clear the root's ground.
  ui.Color? get color => _color;
  ui.Color? _color;
  set color(ui.Color? value) {
    if (value != _color) {
      _color = value;
      markNeedsAddToScene();
    }
  }

  /// At most four explicit view-root logical fills, exclusive with [color].
  List<ui.AvioOutputGroundRegion> get regions => _regions;
  List<ui.AvioOutputGroundRegion> _regions;
  set regions(List<ui.AvioOutputGroundRegion> value) {
    if (!_sameGroundRegions(value, _regions)) {
      _regions = _copyGroundRegions(value);
      markNeedsAddToScene();
    }
  }

  @override
  void addToScene(ui.SceneBuilder builder) {
    engineLayer = builder.pushAvioOutputGround(
      color: color,
      regions: regions,
      offset: offset,
      oldLayer: engineLayer as ui.AvioOutputGroundEngineLayer?,
    );
    addChildrenToScene(builder);
    builder.pop();
  }
}

/// Root alpha which changes only the composited layer, retaining child paint.
class RenderAvioItemEffect extends RenderProxyBox {
  /// Creates a root-only alpha author.
  RenderAvioItemEffect({double? opacity = 1, int declarationId = 1, RenderBox? child})
    : _opacity = opacity,
      _declarationId = declarationId,
      super(child) {
    if (declarationId <= 0) throw RangeError.value(declarationId, 'declarationId');
    if (opacity != null && (!opacity.isFinite || opacity < 0 || opacity > 1)) {
      throw RangeError.value(opacity, 'opacity');
    }
  }

  /// Alpha for the content revision.
  double? get opacity => _opacity;
  double? _opacity;
  set opacity(double? value) {
    if (value != null && (!value.isFinite || value < 0 || value > 1)) {
      throw RangeError.value(value, 'opacity');
    }
    if (_opacity != value) {
      _opacity = value;
      markNeedsCompositedLayerUpdate();
    }
  }

  /// Exact declaration identity; changes do not repaint the child.
  int get declarationId => _declarationId;
  int _declarationId;
  set declarationId(int value) {
    if (value <= 0) throw RangeError.value(value, 'declarationId');
    if (_declarationId != value) {
      _declarationId = value;
      markNeedsCompositedLayerUpdate();
    }
  }

  @override
  bool get alwaysNeedsCompositing => true;
  @override
  bool get isRepaintBoundary => true;

  @override
  OffsetLayer updateCompositedLayer({required covariant AvioItemEffectLayer? oldLayer}) {
    assert(opacity == null || _debugRootPosition(this), 'AvioItemEffect must be at the view root.');
    return (oldLayer ?? AvioItemEffectLayer())
      ..opacity = opacity
      ..declarationId = declarationId;
  }
}

/// Root output fill carried atomically beside this root's pixel/empty revision.
class RenderAvioOutputGround extends RenderProxyBox {
  /// Creates a root-only ground author.
  RenderAvioOutputGround({
    ui.Color? color,
    List<ui.AvioOutputGroundRegion> regions = const <ui.AvioOutputGroundRegion>[],
    RenderBox? child,
  }) : _color = color,
       _regions = _copyGroundRegions(regions),
       super(child);

  /// Fill color; null explicitly clears it.
  ui.Color? get color => _color;
  ui.Color? _color;
  set color(ui.Color? value) {
    if (_color != value) {
      _color = value;
      markNeedsCompositedLayerUpdate();
    }
  }

  /// Explicit logical root regions, with a null [color].
  List<ui.AvioOutputGroundRegion> get regions => _regions;
  List<ui.AvioOutputGroundRegion> _regions;
  set regions(List<ui.AvioOutputGroundRegion> value) {
    if (!_sameGroundRegions(value, _regions)) {
      _regions = _copyGroundRegions(value);
      markNeedsCompositedLayerUpdate();
    }
  }

  @override
  bool get alwaysNeedsCompositing => true;
  @override
  bool get isRepaintBoundary => true;

  @override
  OffsetLayer updateCompositedLayer({required covariant AvioOutputGroundLayer? oldLayer}) {
    assert(_debugRootPosition(this), 'AvioOutputGround must be at the view root.');
    return (oldLayer ?? AvioOutputGroundLayer())
      ..color = color
      ..regions = regions;
  }
}

/// Retained revision and kind of intended ready root pixels.
class AvioReadyContentLayer extends OffsetLayer {
  ui.AvioReadyContentKind get kind => _kind;
  ui.AvioReadyContentKind _kind;
  set kind(ui.AvioReadyContentKind value) {
    if (_kind != value) {
      _kind = value;
      markNeedsAddToScene();
    }
  }

  /// Creates an author for an intended ready revision.
  AvioReadyContentLayer({
    required int? contentRevision,
    ui.AvioReadyContentKind kind = ui.AvioReadyContentKind.static,
    super.offset,
  }) : _contentRevision = contentRevision,
       _kind = kind {
    if (contentRevision != null && contentRevision <= 0) {
      throw RangeError.value(contentRevision, 'contentRevision');
    }
  }

  /// Authored content identity; a fresh image must use a fresh revision.
  int? get contentRevision => _contentRevision;
  int? _contentRevision;
  set contentRevision(int? value) {
    if (value != null && value <= 0) {
      throw RangeError.value(value, 'contentRevision');
    }
    if (value != _contentRevision) {
      _contentRevision = value;
      markNeedsAddToScene();
    }
  }

  @override
  void addToScene(ui.SceneBuilder builder) {
    final int? revision = contentRevision;
    final ui.EngineLayer? previous = engineLayer;
    if (revision == null) {
      engineLayer = builder.pushOffset(
        offset.dx,
        offset.dy,
        oldLayer: previous is ui.OffsetEngineLayer ? previous : null,
      );
    } else {
      engineLayer = builder.pushAvioReadyContent(
        contentRevision: revision,
        kind: kind,
        offset: offset,
        oldLayer: previous is ui.AvioReadyContentEngineLayer ? previous : null,
      );
    }
    addChildrenToScene(builder);
    builder.pop();
  }
}

/// Revision metadata for intended ready content, retaining its child's painting.
class RenderAvioReadyContent extends RenderProxyBox {
  ui.AvioReadyContentKind get kind => _kind;
  ui.AvioReadyContentKind _kind;
  set kind(ui.AvioReadyContentKind value) {
    if (_kind != value) {
      _kind = value;
      markNeedsCompositedLayerUpdate();
    }
  }

  /// Creates a root-only ready-content author.
  RenderAvioReadyContent({
    required int? contentRevision,
    ui.AvioReadyContentKind kind = ui.AvioReadyContentKind.static,
    RenderBox? child,
  }) : _contentRevision = contentRevision,
       _kind = kind,
       super(child) {
    if (contentRevision != null && contentRevision <= 0) {
      throw RangeError.value(contentRevision, 'contentRevision');
    }
  }

  /// Nonzero identity of this intended ready root content.
  int? get contentRevision => _contentRevision;
  int? _contentRevision;
  set contentRevision(int? value) {
    if (value != null && value <= 0) {
      throw RangeError.value(value, 'contentRevision');
    }
    if (_contentRevision != value) {
      _contentRevision = value;
      markNeedsCompositedLayerUpdate();
    }
  }

  @override
  bool get alwaysNeedsCompositing => true;
  @override
  bool get isRepaintBoundary => true;

  @override
  OffsetLayer updateCompositedLayer({required covariant AvioReadyContentLayer? oldLayer}) {
    assert(
      contentRevision == null || _debugRootPosition(this),
      'AvioReadyContent must be at the view root.',
    );
    return (oldLayer ?? AvioReadyContentLayer(contentRevision: contentRevision))
      ..contentRevision = contentRevision
      ..kind = kind;
  }
}
