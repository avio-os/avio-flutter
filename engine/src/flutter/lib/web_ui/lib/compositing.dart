// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

part of ui;

abstract class Scene {
  Future<Image> toImage(int width, int height);
  Image toImageSync(int width, int height);
  void dispose();
}

abstract class TransformEngineLayer implements EngineLayer {}

abstract class OffsetEngineLayer implements EngineLayer {}

abstract class ClipRectEngineLayer implements EngineLayer {}

abstract class ClipRRectEngineLayer implements EngineLayer {}

abstract class ClipRSuperellipseEngineLayer implements EngineLayer {}

abstract class ClipPathEngineLayer implements EngineLayer {}

abstract class OpacityEngineLayer implements EngineLayer {}

abstract class AvioItemEffectEngineLayer implements EngineLayer {}

class AvioOutputGroundRegion {
  const AvioOutputGroundRegion({required this.rect, required this.color});
  final Rect rect;
  final Color color;
}

abstract class AvioOutputGroundEngineLayer implements EngineLayer {}

/// Whether ready root pixels are immutable or remain live.
enum AvioReadyContentKind { static, live }

abstract class AvioReadyContentEngineLayer implements EngineLayer {}

/// What an Avio external compositor may route to a claiming view.
enum AvioHitRegionKind { claim, outputCapture }

abstract class AvioHitRegionEngineLayer implements EngineLayer {}

abstract class ColorFilterEngineLayer implements EngineLayer {}

abstract class ImageFilterEngineLayer implements EngineLayer {}

abstract class BackdropFilterEngineLayer implements EngineLayer {}

abstract class ShaderMaskEngineLayer implements EngineLayer {}

abstract class SceneBuilder {
  factory SceneBuilder() => engine.renderer.createSceneBuilder();

  AvioHitRegionEngineLayer pushAvioHitRegion({
    required Rect rect,
    bool enabled = true,
    AvioHitRegionKind kind = AvioHitRegionKind.claim,
    Offset offset = Offset.zero,
    AvioHitRegionEngineLayer? oldLayer,
  });
  AvioItemEffectEngineLayer pushAvioItemEffect({
    required double opacity,
    int declarationId = 1,
    Offset offset = Offset.zero,
    AvioItemEffectEngineLayer? oldLayer,
  });
  AvioOutputGroundEngineLayer pushAvioOutputGround({
    required Color? color,
    List<AvioOutputGroundRegion> regions = const <AvioOutputGroundRegion>[],
    Offset offset = Offset.zero,
    AvioOutputGroundEngineLayer? oldLayer,
  });

  AvioReadyContentEngineLayer pushAvioReadyContent({
    required int contentRevision,
    AvioReadyContentKind kind = AvioReadyContentKind.static,
    Offset offset = Offset.zero,
    AvioReadyContentEngineLayer? oldLayer,
  });

  OffsetEngineLayer pushOffset(double dx, double dy, {OffsetEngineLayer? oldLayer});
  TransformEngineLayer pushTransform(Float64List matrix4, {TransformEngineLayer? oldLayer});
  ClipRectEngineLayer pushClipRect(
    Rect rect, {
    Clip clipBehavior = Clip.antiAlias,
    ClipRectEngineLayer? oldLayer,
  });
  ClipRRectEngineLayer pushClipRRect(
    RRect rrect, {
    required Clip clipBehavior,
    ClipRRectEngineLayer? oldLayer,
  });
  ClipRSuperellipseEngineLayer pushClipRSuperellipse(
    RSuperellipse rsuperellipse, {
    required Clip clipBehavior,
    ClipRSuperellipseEngineLayer? oldLayer,
  });
  ClipPathEngineLayer pushClipPath(
    Path path, {
    Clip clipBehavior = Clip.antiAlias,
    ClipPathEngineLayer? oldLayer,
  });
  OpacityEngineLayer pushOpacity(
    int alpha, {
    Offset offset = Offset.zero,
    OpacityEngineLayer? oldLayer,
  });
  ColorFilterEngineLayer pushColorFilter(ColorFilter filter, {ColorFilterEngineLayer? oldLayer});
  ImageFilterEngineLayer pushImageFilter(
    ImageFilter filter, {
    Offset offset = Offset.zero,
    ImageFilterEngineLayer? oldLayer,
  });
  BackdropFilterEngineLayer pushBackdropFilter(
    ImageFilter filter, {
    BlendMode blendMode = BlendMode.srcOver,
    BackdropFilterEngineLayer? oldLayer,
    int? backdropId,
  });
  ShaderMaskEngineLayer pushShaderMask(
    Shader shader,
    Rect maskRect,
    BlendMode blendMode, {
    ShaderMaskEngineLayer? oldLayer,
    FilterQuality filterQuality = FilterQuality.low,
  });
  void addRetained(EngineLayer retainedLayer);
  void pop();
  void addPerformanceOverlay(int enabledOptions, Rect bounds);
  void addPicture(
    Offset offset,
    Picture picture, {
    bool isComplexHint = false,
    bool willChangeHint = false,
  });
  void addTexture(
    int textureId, {
    Offset offset = Offset.zero,
    double width = 0.0,
    double height = 0.0,
    bool freeze = false,
    FilterQuality filterQuality = FilterQuality.low,
  });
  void addPlatformView(
    int viewId, {
    Offset offset = Offset.zero,
    double width = 0.0,
    double height = 0.0,
  });
  Scene build();
  void setProperties(
    double width,
    double height,
    double insetTop,
    double insetRight,
    double insetBottom,
    double insetLeft,
    bool focusable,
  );
}

class EngineLayer {
  void dispose() {}
}
