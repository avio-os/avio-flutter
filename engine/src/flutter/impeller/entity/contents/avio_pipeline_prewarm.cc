// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/avio_pipeline_prewarm.h"

namespace impeller {
bool PrewarmAvioPipelineKeysInTwoPhases(
    const AvioPipelinePrewarmConfig& config,
    const AvioPipelinePrewarmCallback& queue,
    const AvioPipelinePrewarmCallback& validate) {
  return PrewarmAvioPipelineKeys(config, queue) &&
         PrewarmAvioPipelineKeys(config, validate);
}

namespace {
using Stencil = ContentContextOptions::StencilMode;
using Family = AvioPrewarmPipeline;
using Storage = AvioPrewarmGradientStorage;

bool Warm(const AvioPipelinePrewarmCallback& create,
          Family family,
          const ContentContextOptions& options,
          Storage storage = Storage::kSsbo) {
  return create(family, options, storage, ConicalKind::kConical);
}

// Only native colour draws use path triangulation/stencil and stroke overdraw
// prevention. Filter passes and positively proved 1x sources do not.
bool WarmGeometry(const AvioPipelinePrewarmCallback& create,
                  Family family,
                  ContentContextOptions options,
                  bool triangle_fan,
                  Storage storage = Storage::kSsbo) {
  options.depth_write_enabled = options.blend_mode == BlendMode::kSrc;
  for (auto primitive : {PrimitiveType::kTriangle, PrimitiveType::kTriangleFan,
                         PrimitiveType::kTriangleStrip}) {
    if (primitive == PrimitiveType::kTriangleFan && !triangle_fan) {
      continue;
    }
    options.primitive_type = primitive;
    if (!Warm(create, family, options, storage)) {
      return false;
    }
  }
  options.primitive_type = PrimitiveType::kTriangleStrip;
  options.stencil_mode = Stencil::kCoverCompare;
  if (!Warm(create, family, options, storage)) {
    return false;
  }
  options.stencil_mode = Stencil::kIgnore;
  options.depth_compare = CompareFunction::kGreater;
  options.depth_write_enabled = true;
  return Warm(create, family, options, storage);
}

bool WarmRect(const AvioPipelinePrewarmCallback& create,
              Family family,
              ContentContextOptions options,
              Storage storage = Storage::kSsbo) {
  options.primitive_type = PrimitiveType::kTriangleStrip;
  options.depth_write_enabled = options.blend_mode == BlendMode::kSrc;
  if (!Warm(create, family, options, storage)) {
    return false;
  }
  if (!options.has_depth_stencil_attachments) {
    return true;
  }
  options.stencil_mode = Stencil::kCoverCompare;
  return Warm(create, family, options, storage);
}

bool WarmFastGradient(const AvioPipelinePrewarmCallback& create,
                      ContentContextOptions options) {
  // FastLinearGradient constructs a triangle list, including the cover draw.
  // It never uses the original path's fan/strip or stroke-overdraw key.
  options.primitive_type = PrimitiveType::kTriangle;
  options.depth_write_enabled = options.blend_mode == BlendMode::kSrc;
  if (!Warm(create, Family::kFastGradient, options)) {
    return false;
  }
  options.stencil_mode = Stencil::kCoverCompare;
  return Warm(create, Family::kFastGradient, options);
}

bool WarmClip(const AvioPipelinePrewarmCallback& create,
              ContentContextOptions options,
              bool triangle_fan) {
  options.blend_mode = BlendMode::kDst;
  for (auto primitive : {PrimitiveType::kTriangle, PrimitiveType::kTriangleFan,
                         PrimitiveType::kTriangleStrip}) {
    if (primitive == PrimitiveType::kTriangleFan && !triangle_fan) {
      continue;
    }
    options.primitive_type = primitive;
    for (auto stencil :
         {Stencil::kStencilNonZeroFill, Stencil::kStencilEvenOddFill,
          Stencil::kStencilIncrementAll}) {
      options.stencil_mode = stencil;
      if (!Warm(create, Family::kClip, options)) {
        return false;
      }
    }
    options.stencil_mode = Stencil::kIgnore;
    options.depth_write_enabled = true;
    if (!Warm(create, Family::kClip, options)) {
      return false;
    }
    options.depth_write_enabled = false;
  }
  options.primitive_type = PrimitiveType::kTriangleStrip;
  options.depth_write_enabled = true;
  for (auto stencil :
       {Stencil::kCoverCompare, Stencil::kCoverCompareInverted}) {
    options.stencil_mode = stencil;
    if (!Warm(create, Family::kClip, options)) {
      return false;
    }
  }
  return true;
}
}  // namespace

bool PrewarmAvioPipelineKeys(const AvioPipelinePrewarmConfig& config,
                             const AvioPipelinePrewarmCallback& create) {
  if (!create || (config.native_format != PixelFormat::kR8G8B8A8UNormInt &&
                  config.native_format != PixelFormat::kB8G8R8A8UNormInt)) {
    return false;
  }
  ContentContextOptions native;
  native.depth_compare = CompareFunction::kGreaterEqual;
  native.color_attachment_pixel_format = config.native_format;
  native.sample_count = SampleCount::kCount4;
  native.is_stencil_only = config.native_stencil_only;
  for (auto blend : {BlendMode::kSrcOver, BlendMode::kSrc}) {
    auto options = native;
    options.blend_mode = blend;
    if (!WarmGeometry(create, Family::kSolid, options,
                      config.supports_triangle_fan) ||
        !WarmRect(create, Family::kTiledTexture, options) ||
        !WarmFastGradient(create, options)) {
      return false;
    }
    // Text and native path-shadow vertices use triangles without depth writes,
    // even when an opaque entity is reordered to Src.
    options.primitive_type = PrimitiveType::kTriangle;
    if (!Warm(create, Family::kGlyph, options) ||
        !Warm(create, Family::kShadowVertices, options)) {
      return false;
    }
    // Ordinary UberSDF is only used by the proven 1x route. Negotiated arcs and
    // bordered rounded rectangles instead record native4 before continuous
    // replay, independently of EN50.
    if (config.continuous_sdf_cuts &&
        !WarmRect(create, Family::kUberSdf, options)) {
      return false;
    }
    for (auto storage :
         {Storage::kSsbo, Storage::kUniform, Storage::kTexture}) {
      if ((storage == Storage::kSsbo) != config.supports_ssbo) {
        continue;
      }
      // Shell's island rim is a stroked linear-gradient path; its radial light
      // pools are circles/rectangles. Sweep/conical gradients have no current
      // Shell caller and retain the upstream default/lazy variants.
      if (!WarmGeometry(create, Family::kLinearGradient, options,
                        config.supports_triangle_fan, storage) ||
          !WarmRect(create, Family::kRadialGradient, options, storage)) {
        return false;
      }
    }
  }

  // Shell Clear paths include charging-halo strokes and preview cutouts.
  auto cut = native;
  for (auto blend : {BlendMode::kClear, BlendMode::kDstIn, BlendMode::kDstOut,
                     BlendMode::kScreen}) {
    cut.blend_mode = blend;
    if (!WarmGeometry(create, Family::kSolid, cut,
                      config.supports_triangle_fan)) {
      return false;
    }
  }
  for (auto blend : {BlendMode::kDstIn, BlendMode::kScreen}) {
    cut.blend_mode = blend;
    if (!WarmFastGradient(create, cut)) {
      return false;
    }
    for (auto storage :
         {Storage::kSsbo, Storage::kUniform, Storage::kTexture}) {
      if ((storage == Storage::kSsbo) == config.supports_ssbo &&
          !WarmRect(create, Family::kLinearGradient, cut, storage)) {
        return false;
      }
    }
  }
  if (config.continuous_sdf_cuts) {
    cut.blend_mode = BlendMode::kClear;
    if (!WarmRect(create, Family::kUberSdf, cut)) {
      return false;
    }
  }
  if (!WarmClip(create, native, config.supports_triangle_fan)) {
    return false;
  }

  // TextureContents is a rectangle strip. Native seeding and layer restoration
  // also use Src without depth writes; no path/cover texture cross-product.
  for (auto sample : {SampleCount::kCount1, SampleCount::kCount4}) {
    for (auto format :
         {PixelFormat::kR8G8B8A8UNormInt, PixelFormat::kB8G8R8A8UNormInt}) {
      if (sample == SampleCount::kCount4 && format != config.native_format) {
        continue;
      }
      auto options = native;
      options.sample_count = sample;
      options.color_attachment_pixel_format = format;
      options.has_depth_stencil_attachments = sample == SampleCount::kCount4;
      options.is_stencil_only =
          options.has_depth_stencil_attachments && config.native_stencil_only;
      options.primitive_type = PrimitiveType::kTriangleStrip;
      for (auto blend : {BlendMode::kSrcOver, BlendMode::kSrc,
                         BlendMode::kDstIn, BlendMode::kScreen}) {
        options.blend_mode = blend;
        options.depth_write_enabled = blend == BlendMode::kSrc;
        for (auto family : {Family::kTexture, Family::kStrictTexture}) {
          if (!Warm(create, family, options)) {
            return false;
          }
          if (blend == BlendMode::kSrc) {
            auto seed = options;
            seed.depth_write_enabled = false;
            if (!Warm(create, family, seed)) {
              return false;
            }
          }
        }
      }
      if (sample == SampleCount::kCount1) {
        for (auto blend : {BlendMode::kSrcOver, BlendMode::kSrc}) {
          options.blend_mode = blend;
          if (!WarmRect(create, Family::kSolid, options)) {
            return false;
          }
          // The direct proof admits uniform UberSDF shapes, including only
          // symmetric nonempty superellipses. ComplexRSE has no direct-root
          // consumer and is not prewarmed as a speculative root-format key.
          if (config.use_sdfs && !WarmRect(create, Family::kUberSdf, options)) {
            return false;
          }
        }
      }
    }
  }

  // These filter callbacks use strips and never request depth writes. Only the
  // native-format layer bank is used by explicit 1x filter subpasses; foreign
  // RGBA/BGRA root formats above are needed for image/SDF draws and copy-back.
  for (auto sample : {SampleCount::kCount1, SampleCount::kCount4}) {
    auto options = native;
    options.sample_count = sample;
    options.has_depth_stencil_attachments = sample == SampleCount::kCount4;
    options.is_stencil_only =
        options.has_depth_stencil_attachments && config.native_stencil_only;
    options.primitive_type = PrimitiveType::kTriangleStrip;
    for (auto blend : {BlendMode::kSrcOver, BlendMode::kSrc}) {
      options.blend_mode = blend;
      for (auto family :
           {Family::kBorderMaskBlur, Family::kColorMatrix,
            Family::kLinearToSrgb, Family::kSrgbToLinear, Family::kYuvToRgb}) {
        if (!Warm(create, family, options)) {
          return false;
        }
      }
      if (sample == SampleCount::kCount4) {
        for (auto family : {Family::kRRectBlur, Family::kRSuperellipseBlur}) {
          if (!Warm(create, family, options)) {
            return false;
          }
        }
      }
    }
    if (sample == SampleCount::kCount1) {
      options.blend_mode = BlendMode::kSrcOver;
      for (auto family : {Family::kGaussianBlur, Family::kMorphology,
                          Family::kDownsample, Family::kDownsampleBounded}) {
        if (!Warm(create, family, options)) {
          return false;
        }
      }
      if (config.needs_gles_decal_downsample &&
          !Warm(create, Family::kDownsampleGles, options)) {
        return false;
      }
    } else {
      options.blend_mode = BlendMode::kClear;
      options.is_for_rrect_blur_clear = true;
      for (auto family : {Family::kRRectBlur, Family::kRSuperellipseBlur}) {
        if (!Warm(create, family, options)) {
          return false;
        }
      }
    }
  }
  return true;
}
}  // namespace impeller
