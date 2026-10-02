// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/content_context.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <memory>
#include <utility>
#include <vector>

#include "flutter/display_list/image/dl_image.h"
#include "fml/closure.h"
#include "fml/trace_event.h"
#include "impeller/base/validation.h"
#include "impeller/core/formats.h"
#include "impeller/core/texture_descriptor.h"
#include "impeller/entity/avio_coverage_region.h"
#include "impeller/entity/contents/avio_pipeline_prewarm.h"
#include "impeller/entity/contents/coverage_path_atlas.h"
#include "impeller/entity/contents/framebuffer_blend_contents.h"
#include "impeller/entity/contents/pipelines.h"
#include "impeller/entity/contents/porter_duff_blend_coefficients.h"
#include "impeller/entity/contents/text_shadow_cache.h"
#include "impeller/entity/coverage_tiled_render_pass.h"
#include "impeller/entity/entity.h"
#include "impeller/entity/render_target_cache.h"
#include "impeller/renderer/command_buffer.h"
#include "impeller/renderer/continuous_coverage_pipeline.h"
#include "impeller/renderer/pipeline.h"
#include "impeller/renderer/pipeline_descriptor.h"
#include "impeller/renderer/pipeline_library.h"
#include "impeller/renderer/render_resource_scope.h"
#include "impeller/renderer/render_target.h"
#include "impeller/renderer/sampler_library.h"
#include "impeller/renderer/shader_function.h"
#include "impeller/renderer/texture_util.h"
#include "impeller/tessellator/tessellator.h"
#include "impeller/typographer/typographer_context.h"

namespace impeller {

namespace {

/// A generic version of `Variants` which mostly exists to reduce code size.
class GenericVariants {
 public:
  void Set(const ContentContextOptions& options,
           std::unique_ptr<GenericRenderPipelineHandle> pipeline) {
    uint64_t p_key = options.ToKey();
    for (const auto& [key, pipeline] : pipelines_) {
      if (key == p_key) {
        return;
      }
    }
    pipelines_.push_back(std::make_pair(p_key, std::move(pipeline)));
  }

  void SetDefault(const ContentContextOptions& options,
                  std::unique_ptr<GenericRenderPipelineHandle> pipeline) {
    default_options_ = options;
    if (pipeline) {
      Set(options, std::move(pipeline));
    }
  }

  GenericRenderPipelineHandle* Get(const ContentContextOptions& options) const {
    uint64_t p_key = options.ToKey();
    for (const auto& [key, pipeline] : pipelines_) {
      if (key == p_key) {
        return pipeline.get();
      }
    }
    return nullptr;
  }

  void SetDefaultDescriptor(std::optional<PipelineDescriptor> desc) {
    desc_ = std::move(desc);
  }

  size_t GetPipelineCount() const { return pipelines_.size(); }

  bool IsDefault(const ContentContextOptions& opts) {
    return default_options_.has_value() &&
           opts.ToKey() == default_options_.value().ToKey();
  }

 protected:
  std::optional<PipelineDescriptor> desc_;
  std::optional<ContentContextOptions> default_options_;
  std::vector<std::pair<uint64_t, std::unique_ptr<GenericRenderPipelineHandle>>>
      pipelines_;
};

/// Holds multiple Pipelines associated with the same PipelineHandle types.
///
/// For example, it may have multiple
/// RenderPipelineHandle<SolidFillVertexShader, SolidFillFragmentShader>
/// instances for different blend modes. From them you can access the
/// Pipeline.
///
/// See also:
///  - impeller::ContentContextOptions - options from which variants are
///    created.
///  - impeller::Pipeline::CreateVariant
///  - impeller::RenderPipelineHandle<> - The type of objects this typically
///    contains.
template <class PipelineHandleT>
class Variants : public GenericVariants {
  static_assert(
      ShaderStageCompatibilityChecker<
          typename PipelineHandleT::VertexShader,
          typename PipelineHandleT::FragmentShader>::Check(),
      "The output slots for the fragment shader don't have matches in the "
      "vertex shader's output slots. This will result in a linker error.");

 public:
  Variants() = default;

  void Set(const ContentContextOptions& options,
           std::unique_ptr<PipelineHandleT> pipeline) {
    GenericVariants::Set(options, std::move(pipeline));
  }

  void SetDefault(const ContentContextOptions& options,
                  std::unique_ptr<PipelineHandleT> pipeline) {
    GenericVariants::SetDefault(options, std::move(pipeline));
  }

  void CreateDefault(const Context& context,
                     const ContentContextOptions& options,
                     const std::vector<Scalar>& constants = {}) {
    std::optional<PipelineDescriptor> desc =
        PipelineHandleT::Builder::MakeDefaultPipelineDescriptor(context,
                                                                constants);
    if (!desc.has_value()) {
      VALIDATION_LOG << "Failed to create default pipeline.";
      return;
    }
    context.GetPipelineLibrary()->LogPipelineCreation(*desc);
    options.ApplyToPipelineDescriptor(*desc);
    desc_ = desc;
    SetDefault(options, std::make_unique<PipelineHandleT>(context, desc_,
                                                          /*async=*/true));
  }

  PipelineHandleT* Get(const ContentContextOptions& options) const {
    return static_cast<PipelineHandleT*>(GenericVariants::Get(options));
  }

  /// Compiles the variant for `options` asynchronously, ahead of its first
  /// use. Its descriptor is the default descriptor with `options` applied,
  /// the same one `CreateIfNeeded` derives from the compiled default, so the
  /// first use finds this handle instead of compiling the variant on the
  /// calling thread. The label marks it as prewarmed.
  void Prewarm(const Context& context, const ContentContextOptions& options) {
    if (!desc_.has_value() || IsDefault(options) || Get(options) != nullptr) {
      return;
    }
    PipelineDescriptor desc = desc_.value();
    options.ApplyToPipelineDescriptor(desc);
    desc.SetLabel(
        std::format("{} V#{} Prewarmed", desc.GetLabel(), GetPipelineCount()));
    Set(options,
        std::make_unique<PipelineHandleT>(context, desc, /*async=*/true));
  }

  PipelineHandleT* GetDefault(const Context& context) {
    if (!default_options_.has_value()) {
      return nullptr;
    }
    PipelineHandleT* result = Get(default_options_.value());
    if (result != nullptr) {
      return result;
    }
    SetDefault(default_options_.value(), std::make_unique<PipelineHandleT>(
                                             context, desc_, /*async=*/false));
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return Get(default_options_.value());
  }

 private:
  Variants(const Variants&) = delete;

  Variants& operator=(const Variants&) = delete;
};

template <class RenderPipelineHandleT>
RenderPipelineHandleT* CreateIfNeeded(
    const ContentContext* context,
    Variants<RenderPipelineHandleT>& container,
    ContentContextOptions opts,
    PipelineCompileQueue* compile_queue) {
  if (!context->CanCreatePipelines()) {
    return nullptr;
  }

  if (RenderPipelineHandleT* found = container.Get(opts)) {
    return found;
  }

  RenderPipelineHandleT* default_handle =
      container.GetDefault(*context->GetContext());
  if (container.IsDefault(opts)) {
    return default_handle;
  }

  // The default must always be initialized in the constructor.
  FML_CHECK(default_handle != nullptr);

  const std::shared_ptr<Pipeline<PipelineDescriptor>>& pipeline =
      default_handle->WaitAndGet(compile_queue);
  if (!pipeline) {
    return nullptr;
  }

  auto variant_future = pipeline->CreateVariant(
      /*async=*/false, [&opts, variants_count = container.GetPipelineCount()](
                           PipelineDescriptor& desc) {
        opts.ApplyToPipelineDescriptor(desc);
        desc.SetLabel(std::format("{} V#{}", desc.GetLabel(), variants_count));
      });
  std::unique_ptr<RenderPipelineHandleT> variant =
      std::make_unique<RenderPipelineHandleT>(std::move(variant_future));
  container.Set(opts, std::move(variant));
  return container.Get(opts);
}

template <class TypedPipeline>
PipelineRef GetPipeline(const ContentContext* context,
                        Variants<TypedPipeline>& container,
                        ContentContextOptions opts) {
  auto compile_queue =
      context->GetContext()->GetPipelineLibrary()->GetPipelineCompileQueue();
  TypedPipeline* pipeline =
      CreateIfNeeded(context, container, opts, compile_queue);
  if (!pipeline) {
    return raw_ptr<Pipeline<PipelineDescriptor>>();
  }
  auto result = pipeline->WaitAndGet(compile_queue);
  if (result &&
      context->GetContext()->GetAvioAntialiasingConfig().UsesCoverage() &&
      !IsAvioRasterFrameActive()) {
    if (!context->WarmAvioPipelineVariants(result)) {
      return raw_ptr<Pipeline<PipelineDescriptor>>();
    }
  }
  return raw_ptr(result);
}

}  // namespace

struct ContentContext::Pipelines {
  // clang-format off
  Variants<BlendColorBurnPipeline> blend_colorburn;
  Variants<BlendColorDodgePipeline> blend_colordodge;
  Variants<BlendColorPipeline> blend_color;
  Variants<BlendDarkenPipeline> blend_darken;
  Variants<BlendDifferencePipeline> blend_difference;
  Variants<BlendExclusionPipeline> blend_exclusion;
  Variants<BlendHardLightPipeline> blend_hardlight;
  Variants<BlendHuePipeline> blend_hue;
  Variants<BlendLightenPipeline> blend_lighten;
  Variants<BlendLuminosityPipeline> blend_luminosity;
  Variants<BlendMultiplyPipeline> blend_multiply;
  Variants<BlendOverlayPipeline> blend_overlay;
  Variants<BlendSaturationPipeline> blend_saturation;
  Variants<BlendScreenPipeline> blend_screen;
  Variants<BlendSoftLightPipeline> blend_softlight;
  Variants<BorderMaskBlurPipeline> border_mask_blur;
  Variants<CirclePipeline> circle;
  Variants<ClipPipeline> clip;
#ifdef IMPELLER_ENABLE_VULKAN
  Variants<CoverageMaskPipeline> coverage_mask;
  Variants<CoverageQuadPipeline> coverage_quad;
#endif
  Variants<ColorMatrixColorFilterPipeline> color_matrix_color_filter;
  Variants<ConicalGradientFillConicalPipeline> conical_gradient_fill;
  Variants<ConicalGradientFillRadialPipeline> conical_gradient_fill_radial;
  Variants<ConicalGradientFillStripPipeline> conical_gradient_fill_strip;
  Variants<ConicalGradientFillStripRadialPipeline> conical_gradient_fill_strip_and_radial;
  Variants<ConicalGradientSSBOFillPipeline> conical_gradient_ssbo_fill;
  Variants<ConicalGradientSSBOFillPipeline> conical_gradient_ssbo_fill_radial;
  Variants<ConicalGradientSSBOFillPipeline> conical_gradient_ssbo_fill_strip_and_radial;
  Variants<ConicalGradientSSBOFillPipeline> conical_gradient_ssbo_fill_strip;
  Variants<ConicalGradientUniformFillConicalPipeline> conical_gradient_uniform_fill;
  Variants<ConicalGradientUniformFillRadialPipeline> conical_gradient_uniform_fill_radial;
  Variants<ConicalGradientUniformFillStripPipeline> conical_gradient_uniform_fill_strip;
  Variants<ConicalGradientUniformFillStripRadialPipeline> conical_gradient_uniform_fill_strip_and_radial;
  Variants<FastGradientPipeline> fast_gradient;
  Variants<FramebufferBlendColorBurnPipeline> framebuffer_blend_colorburn;
  Variants<FramebufferBlendColorDodgePipeline> framebuffer_blend_colordodge;
  Variants<FramebufferBlendColorPipeline> framebuffer_blend_color;
  Variants<FramebufferBlendDarkenPipeline> framebuffer_blend_darken;
  Variants<FramebufferBlendDifferencePipeline> framebuffer_blend_difference;
  Variants<FramebufferBlendExclusionPipeline> framebuffer_blend_exclusion;
  Variants<FramebufferBlendHardLightPipeline> framebuffer_blend_hardlight;
  Variants<FramebufferBlendHuePipeline> framebuffer_blend_hue;
  Variants<FramebufferBlendLightenPipeline> framebuffer_blend_lighten;
  Variants<FramebufferBlendLuminosityPipeline> framebuffer_blend_luminosity;
  Variants<FramebufferBlendMultiplyPipeline> framebuffer_blend_multiply;
  Variants<FramebufferBlendOverlayPipeline> framebuffer_blend_overlay;
  Variants<FramebufferBlendSaturationPipeline> framebuffer_blend_saturation;
  Variants<FramebufferBlendScreenPipeline> framebuffer_blend_screen;
  Variants<FramebufferBlendSoftLightPipeline> framebuffer_blend_softlight;
  Variants<GaussianBlurPipeline> gaussian_blur;
  Variants<GlyphAtlasPipeline> glyph_atlas;
  Variants<LinearGradientFillPipeline> linear_gradient_fill;
  Variants<LinearGradientSSBOFillPipeline> linear_gradient_ssbo_fill;
  Variants<LinearGradientUniformFillPipeline> linear_gradient_uniform_fill;
  Variants<LinearToSrgbFilterPipeline> linear_to_srgb_filter;
  Variants<MorphologyFilterPipeline> morphology_filter;
  Variants<PorterDuffBlendPipeline> clear_blend;
  Variants<PorterDuffBlendPipeline> destination_a_top_blend;
  Variants<PorterDuffBlendPipeline> destination_blend;
  Variants<PorterDuffBlendPipeline> destination_in_blend;
  Variants<PorterDuffBlendPipeline> destination_out_blend;
  Variants<PorterDuffBlendPipeline> destination_over_blend;
  Variants<PorterDuffBlendPipeline> modulate_blend;
  Variants<PorterDuffBlendPipeline> plus_blend;
  Variants<PorterDuffBlendPipeline> screen_blend;
  Variants<PorterDuffBlendPipeline> source_a_top_blend;
  Variants<PorterDuffBlendPipeline> source_blend;
  Variants<PorterDuffBlendPipeline> source_in_blend;
  Variants<PorterDuffBlendPipeline> source_out_blend;
  Variants<PorterDuffBlendPipeline> source_over_blend;
  Variants<PorterDuffBlendPipeline> xor_blend;
  Variants<RadialGradientFillPipeline> radial_gradient_fill;
  Variants<RadialGradientSSBOFillPipeline> radial_gradient_ssbo_fill;
  Variants<RadialGradientUniformFillPipeline> radial_gradient_uniform_fill;
  Variants<RRectBlurPipeline> rrect_blur;
  Variants<RSuperellipseBlurPipeline> rsuperellipse_blur;
  Variants<ShadowVerticesShader> shadow_vertices_;
  Variants<SolidFillPipeline> solid_fill;
  Variants<SrgbToLinearFilterPipeline> srgb_to_linear_filter;
  Variants<SweepGradientFillPipeline> sweep_gradient_fill;
  Variants<SweepGradientSSBOFillPipeline> sweep_gradient_ssbo_fill;
  Variants<SweepGradientUniformFillPipeline> sweep_gradient_uniform_fill;
  Variants<TextureDownsamplePipeline> texture_downsample;
  Variants<TextureDownsampleBoundedPipeline> texture_downsample_bounded;
  Variants<TexturePipeline> texture;
  Variants<TextureStrictSrcPipeline> texture_strict_src;
  Variants<TiledTexturePipeline> tiled_texture;
  Variants<VerticesUber1Shader> vertices_uber_1_;
  Variants<VerticesUber2Shader> vertices_uber_2_;
  Variants<UberSDFPipeline> uber_sdf;
  Variants<UberSDFSSBOPipeline> uber_sdf_ssbo;
  Variants<ComplexRSEPipeline> complex_rse;
  Variants<YUVToRGBFilterPipeline> yuv_to_rgb_filter;

// Web doesn't support external texture OpenGL extensions
#if defined(IMPELLER_ENABLE_OPENGLES) && !defined(FML_OS_EMSCRIPTEN)
  Variants<TiledTextureExternalPipeline> tiled_texture_external;
  Variants<TiledTextureUvExternalPipeline> tiled_texture_uv_external;
#endif

#if defined(IMPELLER_ENABLE_OPENGLES)
  Variants<TextureDownsampleGlesPipeline> texture_downsample_gles;
#endif  // IMPELLER_ENABLE_OPENGLES
  // clang-format on
};

void ContentContextOptions::ApplyToPipelineDescriptor(
    PipelineDescriptor& desc) const {
  auto pipeline_blend = blend_mode;
  if (blend_mode > Entity::kLastPipelineBlendMode) {
    VALIDATION_LOG << "Cannot use blend mode " << static_cast<int>(blend_mode)
                   << " as a pipeline blend.";
    pipeline_blend = BlendMode::kSrcOver;
  }

  desc.SetSampleCount(sample_count);

  ColorAttachmentDescriptor color0 = *desc.GetColorAttachmentDescriptor(0u);
  color0.format = color_attachment_pixel_format;
  color0.alpha_blend_op = BlendOperation::kAdd;
  color0.color_blend_op = BlendOperation::kAdd;
  color0.write_mask = ColorWriteMaskBits::kAll;

  switch (pipeline_blend) {
    case BlendMode::kClear:
      if (is_for_rrect_blur_clear) {
        color0.alpha_blend_op = BlendOperation::kReverseSubtract;
        color0.color_blend_op = BlendOperation::kReverseSubtract;
        color0.dst_alpha_blend_factor = BlendFactor::kOne;
        color0.dst_color_blend_factor = BlendFactor::kOne;
        color0.src_alpha_blend_factor = BlendFactor::kDestinationColor;
        color0.src_color_blend_factor = BlendFactor::kDestinationColor;
      } else {
        color0.dst_alpha_blend_factor = BlendFactor::kZero;
        color0.dst_color_blend_factor = BlendFactor::kZero;
        color0.src_alpha_blend_factor = BlendFactor::kZero;
        color0.src_color_blend_factor = BlendFactor::kZero;
      }
      break;
    case BlendMode::kSrc:
      color0.blending_enabled = false;
      color0.dst_alpha_blend_factor = BlendFactor::kZero;
      color0.dst_color_blend_factor = BlendFactor::kZero;
      color0.src_alpha_blend_factor = BlendFactor::kOne;
      color0.src_color_blend_factor = BlendFactor::kOne;
      break;
    case BlendMode::kDst:
      color0.dst_alpha_blend_factor = BlendFactor::kOne;
      color0.dst_color_blend_factor = BlendFactor::kOne;
      color0.src_alpha_blend_factor = BlendFactor::kZero;
      color0.src_color_blend_factor = BlendFactor::kZero;
      color0.write_mask = ColorWriteMaskBits::kNone;
      break;
    case BlendMode::kSrcOver:
      color0.dst_alpha_blend_factor = BlendFactor::kOneMinusSourceAlpha;
      color0.dst_color_blend_factor = BlendFactor::kOneMinusSourceAlpha;
      color0.src_alpha_blend_factor = BlendFactor::kOne;
      color0.src_color_blend_factor = BlendFactor::kOne;
      break;
    case BlendMode::kDstOver:
      color0.dst_alpha_blend_factor = BlendFactor::kOne;
      color0.dst_color_blend_factor = BlendFactor::kOne;
      color0.src_alpha_blend_factor = BlendFactor::kOneMinusDestinationAlpha;
      color0.src_color_blend_factor = BlendFactor::kOneMinusDestinationAlpha;
      break;
    case BlendMode::kSrcIn:
      color0.dst_alpha_blend_factor = BlendFactor::kZero;
      color0.dst_color_blend_factor = BlendFactor::kZero;
      color0.src_alpha_blend_factor = BlendFactor::kDestinationAlpha;
      color0.src_color_blend_factor = BlendFactor::kDestinationAlpha;
      break;
    case BlendMode::kDstIn:
      color0.dst_alpha_blend_factor = BlendFactor::kSourceAlpha;
      color0.dst_color_blend_factor = BlendFactor::kSourceAlpha;
      color0.src_alpha_blend_factor = BlendFactor::kZero;
      color0.src_color_blend_factor = BlendFactor::kZero;
      break;
    case BlendMode::kSrcOut:
      color0.dst_alpha_blend_factor = BlendFactor::kZero;
      color0.dst_color_blend_factor = BlendFactor::kZero;
      color0.src_alpha_blend_factor = BlendFactor::kOneMinusDestinationAlpha;
      color0.src_color_blend_factor = BlendFactor::kOneMinusDestinationAlpha;
      break;
    case BlendMode::kDstOut:
      color0.dst_alpha_blend_factor = BlendFactor::kOneMinusSourceAlpha;
      color0.dst_color_blend_factor = BlendFactor::kOneMinusSourceAlpha;
      color0.src_alpha_blend_factor = BlendFactor::kZero;
      color0.src_color_blend_factor = BlendFactor::kZero;
      break;
    case BlendMode::kSrcATop:
      color0.dst_alpha_blend_factor = BlendFactor::kOneMinusSourceAlpha;
      color0.dst_color_blend_factor = BlendFactor::kOneMinusSourceAlpha;
      color0.src_alpha_blend_factor = BlendFactor::kDestinationAlpha;
      color0.src_color_blend_factor = BlendFactor::kDestinationAlpha;
      break;
    case BlendMode::kDstATop:
      color0.dst_alpha_blend_factor = BlendFactor::kSourceAlpha;
      color0.dst_color_blend_factor = BlendFactor::kSourceAlpha;
      color0.src_alpha_blend_factor = BlendFactor::kOneMinusDestinationAlpha;
      color0.src_color_blend_factor = BlendFactor::kOneMinusDestinationAlpha;
      break;
    case BlendMode::kXor:
      color0.dst_alpha_blend_factor = BlendFactor::kOneMinusSourceAlpha;
      color0.dst_color_blend_factor = BlendFactor::kOneMinusSourceAlpha;
      color0.src_alpha_blend_factor = BlendFactor::kOneMinusDestinationAlpha;
      color0.src_color_blend_factor = BlendFactor::kOneMinusDestinationAlpha;
      break;
    case BlendMode::kPlus:
      color0.dst_alpha_blend_factor = BlendFactor::kOne;
      color0.dst_color_blend_factor = BlendFactor::kOne;
      color0.src_alpha_blend_factor = BlendFactor::kOne;
      color0.src_color_blend_factor = BlendFactor::kOne;
      break;
    case BlendMode::kModulate:
      color0.dst_alpha_blend_factor = BlendFactor::kSourceAlpha;
      color0.dst_color_blend_factor = BlendFactor::kSourceColor;
      color0.src_alpha_blend_factor = BlendFactor::kZero;
      color0.src_color_blend_factor = BlendFactor::kZero;
      break;
    case BlendMode::kScreen:
      // Premultiplied screen: src + dst * (1 - src). RGB uses the
      // source color coefficient; alpha uses the source alpha coefficient.
      color0.dst_alpha_blend_factor = BlendFactor::kOneMinusSourceAlpha;
      color0.dst_color_blend_factor = BlendFactor::kOneMinusSourceColor;
      color0.src_alpha_blend_factor = BlendFactor::kOne;
      color0.src_color_blend_factor = BlendFactor::kOne;
      break;
    default:
      FML_UNREACHABLE();
  }
  desc.SetColorAttachmentDescriptor(0u, color0);

  if (!has_depth_stencil_attachments) {
    desc.ClearDepthAttachment();
    desc.ClearStencilAttachments();
  } else if (is_stencil_only) {
    desc.ClearDepthAttachment();
    desc.SetStencilPixelFormat(PixelFormat::kS8UInt);
  }

  auto maybe_stencil = desc.GetFrontStencilAttachmentDescriptor();
  auto maybe_depth = desc.GetDepthStencilAttachmentDescriptor();
  FML_DCHECK((has_depth_stencil_attachments && !is_stencil_only) ==
             maybe_depth.has_value())
      << "Depth attachment doesn't match expected pipeline state. "
         "has_depth_stencil_attachments="
      << has_depth_stencil_attachments;
  FML_DCHECK(has_depth_stencil_attachments == maybe_stencil.has_value())
      << "Stencil attachment doesn't match expected pipeline state. "
         "has_depth_stencil_attachments="
      << has_depth_stencil_attachments;
  if (maybe_stencil.has_value()) {
    StencilAttachmentDescriptor front_stencil = maybe_stencil.value();
    StencilAttachmentDescriptor back_stencil = front_stencil;

    switch (stencil_mode) {
      case StencilMode::kIgnore:
        front_stencil.stencil_compare = CompareFunction::kAlways;
        front_stencil.depth_stencil_pass = StencilOperation::kKeep;
        desc.SetStencilAttachmentDescriptors(front_stencil);
        break;
      case StencilMode::kStencilNonZeroFill:
        // The stencil ref should be 0 on commands that use this mode.
        front_stencil.stencil_compare = CompareFunction::kAlways;
        front_stencil.depth_stencil_pass = StencilOperation::kIncrementWrap;
        back_stencil.stencil_compare = CompareFunction::kAlways;
        back_stencil.depth_stencil_pass = StencilOperation::kDecrementWrap;
        desc.SetStencilAttachmentDescriptors(front_stencil, back_stencil);
        break;
      case StencilMode::kStencilEvenOddFill:
        // The stencil ref should be 0 on commands that use this mode.
        front_stencil.stencil_compare = CompareFunction::kEqual;
        front_stencil.depth_stencil_pass = StencilOperation::kIncrementWrap;
        front_stencil.stencil_failure = StencilOperation::kDecrementWrap;
        desc.SetStencilAttachmentDescriptors(front_stencil);
        break;
      case StencilMode::kStencilIncrementAll:
        // The stencil ref should be 0 on commands that use this mode.
        front_stencil.stencil_compare = CompareFunction::kEqual;
        front_stencil.depth_stencil_pass = StencilOperation::kIncrementWrap;
        desc.SetStencilAttachmentDescriptors(front_stencil);
        break;
      case StencilMode::kCoverCompare:
        // The stencil ref should be 0 on commands that use this mode.
        front_stencil.stencil_compare = CompareFunction::kNotEqual;
        front_stencil.depth_stencil_pass =
            StencilOperation::kSetToReferenceValue;
        desc.SetStencilAttachmentDescriptors(front_stencil);
        break;
      case StencilMode::kCoverCompareInverted:
        // The stencil ref should be 0 on commands that use this mode.
        front_stencil.stencil_compare = CompareFunction::kEqual;
        front_stencil.stencil_failure = StencilOperation::kSetToReferenceValue;
        desc.SetStencilAttachmentDescriptors(front_stencil);
        break;
    }
  }
  if (maybe_depth.has_value()) {
    DepthAttachmentDescriptor depth = maybe_depth.value();
    depth.depth_write_enabled = depth_write_enabled;
    depth.depth_compare = depth_compare;
    desc.SetDepthStencilAttachmentDescriptor(depth);
  }

  desc.SetPrimitiveType(primitive_type);
  desc.SetPolygonMode(PolygonMode::kFill);
}

namespace {

// Avio's root targets are DRM ARGB8888 images, which the embedder wraps as
// kB8G8R8A8UNormInt, while offscreens use the context's default color format
// (kR8G8B8A8UNormInt on Vulkan). A variant drawn in both needs both.
constexpr PixelFormat kAvioRootColorFormat = PixelFormat::kB8G8R8A8UNormInt;

// What `OptionsFromPass` starts every draw from in a pass with these
// attachments.
ContentContextOptions PassOptions(SampleCount sample_count,
                                  PixelFormat format,
                                  bool has_depth_stencil_attachments) {
  return ContentContextOptions{
      .sample_count = sample_count,
      .depth_compare = CompareFunction::kGreaterEqual,
      .stencil_mode = ContentContextOptions::StencilMode::kIgnore,
      .color_attachment_pixel_format = format,
      .has_depth_stencil_attachments = has_depth_stencil_attachments,
  };
}

}  // namespace

PrewarmTargets MakeAvioPrewarmTargets(const Capabilities& capabilities) {
  return PrewarmTargets{
      .offscreen_format = capabilities.GetDefaultColorFormat(),
      .root_format = kAvioRootColorFormat,
      .multisampled_passes = capabilities.SupportsOffscreenMSAA(),
      .supports_ssbo = capabilities.SupportsSSBO(),
  };
}

std::vector<PrewarmVariant> MakeAvioPrewarmVariants(
    const PrewarmTargets& targets) {
  using Pipeline = PrewarmVariant::Pipeline;
  std::vector<PrewarmVariant> variants;
  const auto add = [&variants](Pipeline pipeline, ContentContextOptions options,
                               BlendMode blend_mode,
                               PrimitiveType primitive_type,
                               bool depth_write_enabled) {
    options.blend_mode = blend_mode;
    options.primitive_type = primitive_type;
    options.depth_write_enabled = depth_write_enabled;
    const PrewarmVariant variant{.pipeline = pipeline, .options = options};
    if (std::find(variants.begin(), variants.end(), variant) ==
        variants.end()) {
      variants.push_back(variant);
    }
  };
  // UberSDF reads gradient stops from a storage buffer where the backend has
  // one (UberSDFContents::Render).
  const Pipeline uber_sdf =
      targets.supports_ssbo ? Pipeline::kUberSDFSSBO : Pipeline::kUberSDF;
  const SampleCount pass_samples =
      targets.multisampled_passes ? SampleCount::kCount4 : SampleCount::kCount1;

  // The passes Canvas draws into: save layers (CreateRenderTarget) in the
  // offscreen format and the root, multisampled with depth/stencil.
  for (const PixelFormat format :
       {targets.offscreen_format, targets.root_format}) {
    if (format == PixelFormat::kUnknown) {
      continue;
    }
    const ContentContextOptions pass = PassOptions(
        pass_samples, format, /*has_depth_stencil_attachments=*/true);
    // Patch 50: every SDF shape, solid or gradient, over a rect quad.
    add(uber_sdf, pass, BlendMode::kSrcOver, PrimitiveType::kTriangleStrip,
        /*depth_write_enabled=*/false);
    // Patch 52 (b): the masked composite, drawn back as a texture.
    add(Pipeline::kTexture, pass, BlendMode::kSrcOver,
        PrimitiveType::kTriangleStrip, /*depth_write_enabled=*/false);
    // Patch 48's path: the backdrop restored after a Flip. Its stencil is
    // disabled, so it writes no depth (TextureContents::Render).
    add(Pipeline::kTexture, pass, BlendMode::kSrc,
        PrimitiveType::kTriangleStrip, /*depth_write_enabled=*/false);
    // Patch 52 (a): an image or gradient on a rect that contains the clip,
    // drawn directly. An opaque one draws with kSrc and writes depth
    // (ColorSourceContents::DrawGeometry).
    for (const bool opaque : {false, true}) {
      const BlendMode blend_mode =
          opaque ? BlendMode::kSrc : BlendMode::kSrcOver;
      add(Pipeline::kTiledTexture, pass, blend_mode,
          PrimitiveType::kTriangleStrip, opaque);
      add(Pipeline::kFastGradient, pass, blend_mode, PrimitiveType::kTriangle,
          opaque);
      if (targets.supports_ssbo) {
        add(Pipeline::kLinearGradientSSBOFill, pass, blend_mode,
            PrimitiveType::kTriangleStrip, opaque);
        add(Pipeline::kRadialGradientSSBOFill, pass, blend_mode,
            PrimitiveType::kTriangleStrip, opaque);
      }
    }
  }

  // Patch 52 (b): the SDF mask and color source snapshots and the "Pipeline
  // Blend Filter" target are single-sample offscreens without depth/stencil.
  if (targets.offscreen_format != PixelFormat::kUnknown) {
    const ContentContextOptions snapshot =
        PassOptions(SampleCount::kCount1, targets.offscreen_format,
                    /*has_depth_stencil_attachments=*/false);
    add(uber_sdf, snapshot, BlendMode::kSrcOver, PrimitiveType::kTriangleStrip,
        /*depth_write_enabled=*/false);
    // PipelineBlend draws the mask with kSrc, then the color source with
    // kSrcIn.
    add(Pipeline::kTexture, snapshot, BlendMode::kSrc,
        PrimitiveType::kTriangleStrip, /*depth_write_enabled=*/false);
    add(Pipeline::kTexture, snapshot, BlendMode::kSrcIn,
        PrimitiveType::kTriangleStrip, /*depth_write_enabled=*/false);
    // A gradient UberSDF cannot shade (a non-similarity local matrix).
    if (targets.supports_ssbo) {
      add(Pipeline::kLinearGradientSSBOFill, snapshot, BlendMode::kSrcOver,
          PrimitiveType::kTriangleStrip, /*depth_write_enabled=*/false);
      add(Pipeline::kRadialGradientSSBOFill, snapshot, BlendMode::kSrcOver,
          PrimitiveType::kTriangleStrip, /*depth_write_enabled=*/false);
    }
  }
  return variants;
}

template <typename PipelineT>
static std::unique_ptr<PipelineT> CreateDefaultPipeline(
    const Context& context) {
  auto desc = PipelineT::Builder::MakeDefaultPipelineDescriptor(context);
  if (!desc.has_value()) {
    return nullptr;
  }
  // Apply default ContentContextOptions to the descriptor.
  const auto default_color_format =
      context.GetCapabilities()->GetDefaultColorFormat();
  ContentContextOptions{.sample_count = SampleCount::kCount4,
                        .primitive_type = PrimitiveType::kTriangleStrip,
                        .color_attachment_pixel_format = default_color_format}
      .ApplyToPipelineDescriptor(*desc);
  return std::make_unique<PipelineT>(context, desc);
}

ContentContext::ContentContext(
    std::shared_ptr<Context> context,
    std::shared_ptr<TypographerContext> typographer_context,
    std::shared_ptr<RenderTargetAllocator> render_target_allocator)
    : context_(std::move(context)),
      lazy_glyph_atlas_(
          std::make_shared<LazyGlyphAtlas>(std::move(typographer_context))),
      pipelines_(new Pipelines()),
      tessellator_(std::make_shared<Tessellator>(
          context_->GetCapabilities()->Supports32BitPrimitiveIndices())),
      render_target_cache_(render_target_allocator == nullptr
                               ? std::make_shared<RenderTargetCache>(
                                     context_->GetResourceAllocator())
                               : std::move(render_target_allocator)),
      data_host_buffer_(HostBuffer::Create(
          context_->GetResourceAllocator(),
          context_->GetIdleWaiter(),
          context_->GetCapabilities()->GetMinimumUniformAlignment(),
          context_->GetSubmissionTracker())),
      text_shadow_cache_(std::make_unique<TextShadowCache>()) {
  if (!context_ || !context_->IsValid()) {
    return;
  }

  AvioPipelineInitialization::Scope initialization(pipeline_initialization_);

  if (context_->GetAvioAntialiasingConfig().UsesCoverage()) {
    sample4_clip_pool_ = std::make_unique<AvioSample4ClipPool>();
    coverage_recorder_storage_ = CoverageTiledRenderPass::CreateStorage();
    if (!coverage_recorder_storage_) {
      return;
    }
  }

  if (context_->GetAvioAntialiasingConfig().continuous_requested_classes != 0) {
    continuous_clip_pool_ = std::make_unique<AvioContinuousClipPool>();
  }
  if (context_->GetAvioAntialiasingConfig().UsesCoverage() &&
      context_->GetBackendType() == Context::BackendType::kVulkan) {
    const size_t blocks =
        context_->GetAvioAntialiasingConfig().continuous_requested_classes != 0
            ? 3u
            : 1u;
    continuous_data_host_buffer_ = HostBuffer::CreateBounded(
        context_->GetResourceAllocator(), context_->GetIdleWaiter(),
        context_->GetCapabilities()->GetMinimumStorageBufferAlignment(),
        blocks);
    if (!continuous_data_host_buffer_) {
      VALIDATION_LOG << "Could not prewarm fixed continuous shader storage.";
      return;
    }
    SamplerDescriptor nearest;
    auto pixel_prefix = nearest;
    pixel_prefix.mip_filter = MipFilter::kBase;
    if (!context_->GetSamplerLibrary()->GetSampler(nearest) ||
        !context_->GetSamplerLibrary()->GetSampler(pixel_prefix)) {
      VALIDATION_LOG << "Could not prewarm fixed coverage samplers.";
      return;
    }
  }

  // On most backends, indexes and other data can be allocated into the same
  // buffers. However, some backends (namely WebGL) require indexes used in
  // indexed draws to be allocated separately from other data. For those
  // backends, we allocate a separate host buffer just for indexes.
  indexes_host_buffer_ =
      context_->GetCapabilities()->NeedsPartitionedHostBuffer()
          ? HostBuffer::Create(
                context_->GetResourceAllocator(), context_->GetIdleWaiter(),
                context_->GetCapabilities()->GetMinimumUniformAlignment(),
                context_->GetSubmissionTracker())
          : data_host_buffer_;
  {
    TextureDescriptor desc;
    desc.storage_mode = StorageMode::kDevicePrivate;
    desc.format = PixelFormat::kR8G8B8A8UNormInt;
    desc.size = ISize{1, 1};
    empty_texture_ = GetContext()->GetResourceAllocator()->CreateTexture(desc);

    std::array<uint8_t, 4> data = Color::BlackTransparent().ToR8G8B8A8();
    std::shared_ptr<CommandBuffer> cmd_buffer =
        GetContext()->CreateCommandBuffer();
    std::shared_ptr<BlitPass> blit_pass =
        cmd_buffer ? cmd_buffer->CreateBlitPass() : nullptr;
    if (!empty_texture_ || !cmd_buffer || !blit_pass) {
      VALIDATION_LOG << "Failed to create required empty texture resources.";
      return;
    }
    HostBuffer& data_host_buffer = GetTransientsDataBuffer();
    BufferView buffer_view = data_host_buffer.Emplace(data);
    if (!buffer_view || !blit_pass->AddCopy(buffer_view, empty_texture_)) {
      VALIDATION_LOG << "Failed to initialize the required empty texture.";
      return;
    }

    if (!blit_pass->EncodeCommands() || !GetContext()
                                             ->GetCommandQueue()
                                             ->Submit({std::move(cmd_buffer)})
                                             .ok()) {
      VALIDATION_LOG << "Failed to create empty texture.";
      return;
    }
  }

  auto options = ContentContextOptions{
      .sample_count = SampleCount::kCount4,
      .color_attachment_pixel_format =
          context_->GetCapabilities()->GetDefaultColorFormat()};
  auto options_trianglestrip = ContentContextOptions{
      .sample_count = SampleCount::kCount4,
      .primitive_type = PrimitiveType::kTriangleStrip,
      .color_attachment_pixel_format =
          context_->GetCapabilities()->GetDefaultColorFormat()};
  auto options_no_msaa_no_depth_stencil = ContentContextOptions{
      .sample_count = SampleCount::kCount1,
      .primitive_type = PrimitiveType::kTriangleStrip,
      .color_attachment_pixel_format =
          context_->GetCapabilities()->GetDefaultColorFormat(),
      .has_depth_stencil_attachments = false};
  const auto supports_decal = static_cast<Scalar>(
      context_->GetCapabilities()->SupportsDecalSamplerAddressMode());

  // Futures for the following pipelines may block in case the first frame is
  // rendered without the pipelines being ready. Put pipelines that are more
  // likely to be used first.
  {
    pipelines_->glyph_atlas.CreateDefault(
        *context_, options,
        {static_cast<Scalar>(
            GetContext()->GetCapabilities()->GetDefaultGlyphAtlasFormat() ==
            PixelFormat::kA8UNormInt)});
    pipelines_->solid_fill.CreateDefault(*context_, options);
    pipelines_->texture.CreateDefault(*context_, options);
    pipelines_->fast_gradient.CreateDefault(*context_, options);
    pipelines_->circle.CreateDefault(*context_, options);
    const bool continuous_sdf =
        context_->GetAvioAntialiasingConfig().RequestsContinuous(
            AvioContinuousClass::kArc) ||
        context_->GetAvioAntialiasingConfig().RequestsContinuous(
            AvioContinuousClass::kBorderedRoundedRect);
    if (context_->GetFlags().use_sdfs || continuous_sdf) {
      // Negotiated arc/border consumers use UberSDF independently of EN50.
      // UberSDF reads gradient stops from a storage buffer where the backend
      // has one, and a gradient ramp texture otherwise.
      if (context_->GetCapabilities()->SupportsSSBO()) {
        pipelines_->uber_sdf_ssbo.CreateDefault(*context_, options);
      } else {
        pipelines_->uber_sdf.CreateDefault(*context_, options);
      }
    }
    if (context_->GetFlags().use_sdfs) {
      pipelines_->complex_rse.CreateDefault(*context_, options);
    }

    if (context_->GetCapabilities()->SupportsSSBO()) {
      pipelines_->linear_gradient_ssbo_fill.CreateDefault(*context_, options);
      pipelines_->radial_gradient_ssbo_fill.CreateDefault(*context_, options);
      pipelines_->conical_gradient_ssbo_fill.CreateDefault(*context_, options,
                                                           {3.0});
      pipelines_->conical_gradient_ssbo_fill_radial.CreateDefault(
          *context_, options, {1.0});
      pipelines_->conical_gradient_ssbo_fill_strip.CreateDefault(
          *context_, options, {2.0});
      pipelines_->conical_gradient_ssbo_fill_strip_and_radial.CreateDefault(
          *context_, options, {0.0});
      pipelines_->sweep_gradient_ssbo_fill.CreateDefault(*context_, options);
    } else {
      pipelines_->linear_gradient_uniform_fill.CreateDefault(*context_,
                                                             options);
      pipelines_->radial_gradient_uniform_fill.CreateDefault(*context_,
                                                             options);
      pipelines_->conical_gradient_uniform_fill.CreateDefault(*context_,
                                                              options);
      pipelines_->conical_gradient_uniform_fill_radial.CreateDefault(*context_,
                                                                     options);
      pipelines_->conical_gradient_uniform_fill_strip.CreateDefault(*context_,
                                                                    options);
      pipelines_->conical_gradient_uniform_fill_strip_and_radial.CreateDefault(
          *context_, options);
      pipelines_->sweep_gradient_uniform_fill.CreateDefault(*context_, options);

      pipelines_->linear_gradient_fill.CreateDefault(*context_, options);
      pipelines_->radial_gradient_fill.CreateDefault(*context_, options);
      pipelines_->conical_gradient_fill.CreateDefault(*context_, options);
      pipelines_->conical_gradient_fill_radial.CreateDefault(*context_,
                                                             options);
      pipelines_->conical_gradient_fill_strip.CreateDefault(*context_, options);
      pipelines_->conical_gradient_fill_strip_and_radial.CreateDefault(
          *context_, options);
      pipelines_->sweep_gradient_fill.CreateDefault(*context_, options);
    }

    /// Setup default clip pipeline.
    auto clip_pipeline_descriptor =
        ClipPipeline::Builder::MakeDefaultPipelineDescriptor(*context_);
    if (!clip_pipeline_descriptor.has_value()) {
      return;
    }
    ContentContextOptions{
        .sample_count = SampleCount::kCount4,
        .color_attachment_pixel_format =
            context_->GetCapabilities()->GetDefaultColorFormat()}
        .ApplyToPipelineDescriptor(*clip_pipeline_descriptor);
    // Disable write to all color attachments.
    auto clip_color_attachments =
        clip_pipeline_descriptor->GetColorAttachmentDescriptors();
    for (auto& color_attachment : clip_color_attachments) {
      color_attachment.second.write_mask = ColorWriteMaskBits::kNone;
    }
    clip_pipeline_descriptor->SetColorAttachmentDescriptors(
        std::move(clip_color_attachments));
    // Preserve the exact no-colour-write descriptor for async clip variants.
    pipelines_->clip.SetDefaultDescriptor(clip_pipeline_descriptor);
    pipelines_->clip.SetDefault(
        options,
        std::make_unique<ClipPipeline>(*context_, clip_pipeline_descriptor));
    pipelines_->texture_downsample.CreateDefault(
        *context_, options_no_msaa_no_depth_stencil);
    pipelines_->texture_downsample_bounded.CreateDefault(
        *context_, options_no_msaa_no_depth_stencil);
    pipelines_->rrect_blur.CreateDefault(*context_, options_trianglestrip);
    pipelines_->rsuperellipse_blur.CreateDefault(*context_,
                                                 options_trianglestrip);
    pipelines_->texture_strict_src.CreateDefault(*context_, options);
    pipelines_->tiled_texture.CreateDefault(*context_, options,
                                            {supports_decal});
    pipelines_->gaussian_blur.CreateDefault(
        *context_, options_no_msaa_no_depth_stencil, {supports_decal});
    pipelines_->border_mask_blur.CreateDefault(*context_,
                                               options_trianglestrip);
    pipelines_->color_matrix_color_filter.CreateDefault(*context_,
                                                        options_trianglestrip);
    pipelines_->shadow_vertices_.CreateDefault(*context_, options);
    pipelines_->vertices_uber_1_.CreateDefault(*context_, options,
                                               {supports_decal});
    pipelines_->vertices_uber_2_.CreateDefault(*context_, options,
                                               {supports_decal});

    const std::array<std::vector<Scalar>, 15> porter_duff_constants =
        GetPorterDuffSpecConstants(supports_decal);
    pipelines_->clear_blend.CreateDefault(*context_, options_trianglestrip,
                                          porter_duff_constants[0]);
    pipelines_->source_blend.CreateDefault(*context_, options_trianglestrip,
                                           porter_duff_constants[1]);
    pipelines_->destination_blend.CreateDefault(
        *context_, options_trianglestrip, porter_duff_constants[2]);
    pipelines_->source_over_blend.CreateDefault(
        *context_, options_trianglestrip, porter_duff_constants[3]);
    pipelines_->destination_over_blend.CreateDefault(
        *context_, options_trianglestrip, porter_duff_constants[4]);
    pipelines_->source_in_blend.CreateDefault(*context_, options_trianglestrip,
                                              porter_duff_constants[5]);
    pipelines_->destination_in_blend.CreateDefault(
        *context_, options_trianglestrip, porter_duff_constants[6]);
    pipelines_->source_out_blend.CreateDefault(*context_, options_trianglestrip,
                                               porter_duff_constants[7]);
    pipelines_->destination_out_blend.CreateDefault(
        *context_, options_trianglestrip, porter_duff_constants[8]);
    pipelines_->source_a_top_blend.CreateDefault(
        *context_, options_trianglestrip, porter_duff_constants[9]);
    pipelines_->destination_a_top_blend.CreateDefault(
        *context_, options_trianglestrip, porter_duff_constants[10]);
    pipelines_->xor_blend.CreateDefault(*context_, options_trianglestrip,
                                        porter_duff_constants[11]);
    pipelines_->plus_blend.CreateDefault(*context_, options_trianglestrip,
                                         porter_duff_constants[12]);
    pipelines_->modulate_blend.CreateDefault(*context_, options_trianglestrip,
                                             porter_duff_constants[13]);
    pipelines_->screen_blend.CreateDefault(*context_, options_trianglestrip,
                                           porter_duff_constants[14]);
  }

  if (context_->GetCapabilities()->SupportsFramebufferFetch()) {
    pipelines_->framebuffer_blend_color.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kColor), supports_decal});
    pipelines_->framebuffer_blend_colorburn.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kColorBurn), supports_decal});
    pipelines_->framebuffer_blend_colordodge.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kColorDodge), supports_decal});
    pipelines_->framebuffer_blend_darken.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kDarken), supports_decal});
    pipelines_->framebuffer_blend_difference.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kDifference), supports_decal});
    pipelines_->framebuffer_blend_exclusion.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kExclusion), supports_decal});
    pipelines_->framebuffer_blend_hardlight.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kHardLight), supports_decal});
    pipelines_->framebuffer_blend_hue.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kHue), supports_decal});
    pipelines_->framebuffer_blend_lighten.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kLighten), supports_decal});
    pipelines_->framebuffer_blend_luminosity.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kLuminosity), supports_decal});
    pipelines_->framebuffer_blend_multiply.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kMultiply), supports_decal});
    pipelines_->framebuffer_blend_overlay.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kOverlay), supports_decal});
    pipelines_->framebuffer_blend_saturation.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kSaturation), supports_decal});
    pipelines_->framebuffer_blend_screen.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kScreen), supports_decal});
    pipelines_->framebuffer_blend_softlight.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kSoftLight), supports_decal});
  } else {
    pipelines_->blend_color.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kColor), supports_decal});
    pipelines_->blend_colorburn.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kColorBurn), supports_decal});
    pipelines_->blend_colordodge.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kColorDodge), supports_decal});
    pipelines_->blend_darken.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kDarken), supports_decal});
    pipelines_->blend_difference.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kDifference), supports_decal});
    pipelines_->blend_exclusion.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kExclusion), supports_decal});
    pipelines_->blend_hardlight.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kHardLight), supports_decal});
    pipelines_->blend_hue.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kHue), supports_decal});
    pipelines_->blend_lighten.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kLighten), supports_decal});
    pipelines_->blend_luminosity.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kLuminosity), supports_decal});
    pipelines_->blend_multiply.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kMultiply), supports_decal});
    pipelines_->blend_overlay.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kOverlay), supports_decal});
    pipelines_->blend_saturation.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kSaturation), supports_decal});
    pipelines_->blend_screen.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kScreen), supports_decal});
    pipelines_->blend_softlight.CreateDefault(
        *context_, options_trianglestrip,
        {static_cast<Scalar>(BlendSelectValues::kSoftLight), supports_decal});
  }

  pipelines_->morphology_filter.CreateDefault(*context_, options_trianglestrip,
                                              {supports_decal});
  pipelines_->linear_to_srgb_filter.CreateDefault(*context_,
                                                  options_trianglestrip);
  pipelines_->srgb_to_linear_filter.CreateDefault(*context_,
                                                  options_trianglestrip);
  pipelines_->yuv_to_rgb_filter.CreateDefault(*context_, options_trianglestrip);

  if (GetContext()->GetBackendType() == Context::BackendType::kOpenGLES) {
#if defined(IMPELLER_ENABLE_OPENGLES) && !defined(FML_OS_MACOSX) && \
    !defined(FML_OS_EMSCRIPTEN)
    // GLES only shader that is unsupported on macOS and web.
    pipelines_->tiled_texture_external.CreateDefault(*context_, options);
    pipelines_->tiled_texture_uv_external.CreateDefault(*context_, options);
#endif  // !defined(FML_OS_MACOSX)

#if defined(IMPELLER_ENABLE_OPENGLES)
    pipelines_->texture_downsample_gles.CreateDefault(*context_,
                                                      options_trianglestrip);
#endif  // IMPELLER_ENABLE_OPENGLES
  }

  if (UsesAvioCoverage()) {
    std::string error;
    const auto& policy = context_->GetAvioAntialiasingConfig();
    const auto region = context_->InitializeAvioCoverageRegion(
        [&] {
          AvioCoverageRegionConfig config;
          config.coverage_max_bytes = policy.coverage_region_max_bytes;
          config.layer_max_bytes = policy.layer_region_max_bytes;
          config.colour_format =
              context_->GetCapabilities()->GetDefaultColorFormat();
          config.colour_depth_stencil_format =
              context_->GetCapabilities()->GetDefaultDepthStencilFormat();
          if (policy.continuous_requested_classes != 0) {
            config.continuous_destination_prefix = true;
            // D32S8 may require eight physical bytes per texel. Reserve the
            // additional native4 prefix inside the same 8 MiB coverage bank;
            // exact allocation requirements still fail admission if larger.
            config.colour_size = {224, 224};
          }
          if (context_->GetBackendType() == Context::BackendType::kOpenGLES) {
            config.cache_native_masks = false;
            config.require_exact_allocated_bytes = false;
          }
          return AvioCoverageRegion::Create(context_->GetResourceAllocator(),
                                            config, &error);
        },
        [](const AvioCoverageRegion& fixed_region, bool reset_interval) {
          return fixed_region.GetDescriptorResourceReport(reset_interval);
        });
    if (!region) {
      VALIDATION_LOG << "Could not initialize negotiated coverage regions: "
                     << error;
      return;
    }
    if (!region->InitializeColourOnce([&] {
          auto commands = context_->CreateCommandBuffer();
          auto initialize =
              commands
                  ? commands->CreateRenderPass(region->GetColourIslandTarget())
                  : nullptr;
          return initialize && initialize->EncodeCommands() &&
                 context_->EnqueueCommandBuffer(std::move(commands));
        })) {
      VALIDATION_LOG << "Could not initialize the bounded colour island.";
      return;
    }
    if (context_->GetBackendType() == Context::BackendType::kOpenGLES) {
      // GL storage is allocated lazily when attached. Realize every fixed
      // warm layer (and prefix scratch) here, outside the raster-frame scope.
      auto commands = context_->CreateCommandBuffer();
      if (!commands)
        return;
      for (auto target : region->GetWarmLayerTargets()) {
        auto clear = commands->CreateRenderPass(target);
        if (!clear || !clear->EncodeCommands())
          return;
      }
      RenderTarget scratch;
      ColorAttachment colour;
      colour.texture = region->GetColourSeedTexture();
      colour.load_action = LoadAction::kClear;
      colour.store_action = StoreAction::kStore;
      scratch.SetColorAttachment(colour, 0);
      auto clear = commands->CreateRenderPass(scratch);
      if (!clear || !clear->EncodeCommands() ||
          !context_->EnqueueCommandBuffer(std::move(commands)) ||
          !context_->FlushCommandBuffers())
        return;
    }
    if (region->CachesNativeMasks()) {
      coverage_path_atlas_ =
          std::make_unique<CoveragePathAtlas>(region, context_);
      if (!coverage_path_atlas_->Initialize()) {
        VALIDATION_LOG << "Could not initialize native coverage mask samples.";
        return;
      }
#ifdef IMPELLER_ENABLE_VULKAN
      auto descriptor =
          CoverageMaskPipeline::Builder::MakeDefaultPipelineDescriptor(
              *context_);
      if (!descriptor.has_value()) {
        VALIDATION_LOG << "Native four-sample mask shader is unavailable.";
        return;
      }
      auto mask_options = options_trianglestrip;
      mask_options.blend_mode = BlendMode::kDst;
      mask_options.stencil_mode =
          ContentContextOptions::StencilMode::kStencilIncrementAll;
      mask_options.depth_compare = CompareFunction::kGreaterEqual;
      mask_options.ApplyToPipelineDescriptor(*descriptor);
      pipelines_->coverage_mask.SetDefault(
          mask_options,
          std::make_unique<CoverageMaskPipeline>(*context_, descriptor));
      // Native mask replay and binary R8 winding variants are warmed before the
      // first raster frame. No first-use shader compilation is hidden in a
      // draw.
      if (!GetCoverageMaskPipeline(mask_options)) {
        return;
      }
      auto quad_descriptor =
          CoverageQuadPipeline::Builder::MakeDefaultPipelineDescriptor(
              *context_);
      if (!quad_descriptor.has_value()) {
        VALIDATION_LOG << "Native analytic quad shader is unavailable.";
        return;
      }
      mask_options.ApplyToPipelineDescriptor(*quad_descriptor);
      pipelines_->coverage_quad.SetDefault(
          mask_options,
          std::make_unique<CoverageQuadPipeline>(*context_, quad_descriptor));
      if (!GetCoverageQuadPipeline(mask_options)) {
        return;
      }
      // Deep stacks replay through the fixed R8 native4 scratch with borrowed
      // D32S8. Every variant is provisioned before raster, including difference
      // depth writes and the final binary fill. Persistent path masks
      // separately use their original atlas S8-only variants.
      auto scratch_options = mask_options;
      scratch_options.color_attachment_pixel_format = PixelFormat::kR8UNormInt;
      scratch_options.is_stencil_only = false;
      if (!GetCoverageMaskPipeline(scratch_options) ||
          !GetCoverageQuadPipeline(scratch_options)) {
        return;
      }
      for (auto primitive :
           {PrimitiveType::kTriangle, PrimitiveType::kTriangleFan,
            PrimitiveType::kTriangleStrip}) {
        if (primitive == PrimitiveType::kTriangleFan &&
            !GetDeviceCapabilities().SupportsTriangleFan()) {
          continue;
        }
        scratch_options.primitive_type = primitive;
        for (bool writes_depth : {false, true}) {
          scratch_options.depth_write_enabled = writes_depth;
          const auto modes =
              writes_depth
                  ? std::
                        array{ContentContextOptions::StencilMode::kIgnore,
                              ContentContextOptions::StencilMode::kCoverCompare,
                              ContentContextOptions::StencilMode::
                                  kCoverCompareInverted}
                  : std::array{
                        ContentContextOptions::StencilMode::
                            kStencilIncrementAll,
                        ContentContextOptions::StencilMode::kStencilNonZeroFill,
                        ContentContextOptions::StencilMode::
                            kStencilEvenOddFill};
          for (auto mode : modes) {
            if (writes_depth && primitive != PrimitiveType::kTriangleStrip &&
                mode != ContentContextOptions::StencilMode::kIgnore) {
              continue;  // stencil covers are always one triangle-strip quad
            }
            scratch_options.stencil_mode = mode;
            if (!GetClipPipeline(scratch_options)) {
              return;
            }
          }
        }
      }
      scratch_options.primitive_type = PrimitiveType::kTriangleStrip;
      scratch_options.blend_mode = BlendMode::kSrc;
      scratch_options.stencil_mode =
          ContentContextOptions::StencilMode::kIgnore;
      scratch_options.depth_write_enabled = false;
      if (!GetSolidFillPipeline(scratch_options)) {
        return;
      }
      auto writer_options = mask_options;
      writer_options.color_attachment_pixel_format = PixelFormat::kR8UNormInt;
      writer_options.is_stencil_only = true;
      for (auto primitive :
           {PrimitiveType::kTriangle, PrimitiveType::kTriangleFan,
            PrimitiveType::kTriangleStrip}) {
        if (primitive == PrimitiveType::kTriangleFan &&
            !GetDeviceCapabilities().SupportsTriangleFan()) {
          continue;
        }
        writer_options.primitive_type = primitive;
        writer_options.blend_mode = BlendMode::kDst;
        for (auto mode :
             {ContentContextOptions::StencilMode::kStencilNonZeroFill,
              ContentContextOptions::StencilMode::kStencilEvenOddFill}) {
          writer_options.stencil_mode = mode;
          if (!GetClipPipeline(writer_options)) {
            return;
          }
        }
        writer_options.blend_mode = BlendMode::kSrcOver;
        for (auto mode : {ContentContextOptions::StencilMode::kIgnore,
                          ContentContextOptions::StencilMode::kCoverCompare}) {
          writer_options.stencil_mode = mode;
          if (!GetSolidFillPipeline(writer_options)) {
            return;
          }
        }
      }
#endif
    }
    if (!PrewarmAvioCoveragePipelines()) {
      VALIDATION_LOG << "Could not prewarm negotiated coverage pipelines.";
      return;
    }
  }
  // Preserve main's asynchronous SDF catalogue for legacy targets. Coverage
  // uses its own physical target catalogue and validates every queued source
  // and required derivative before publishing Ready.
  if (context_->GetFlags().use_sdfs &&
      !context_->GetAvioAntialiasingConfig().UsesCoverage()) {
    TRACE_EVENT0("impeller", "ContentContext::PrewarmPipelineVariants");
    for (const PrewarmVariant& variant : MakeAvioPrewarmVariants(
             MakeAvioPrewarmTargets(*context_->GetCapabilities()))) {
      PrewarmPipelineVariant(variant);
    }
  }
  InitializeCommonlyUsedShadersIfNeeded();
  initialization.Complete();
}

ContentContext::~ContentContext() = default;

bool ContentContext::IsValid() const {
  return pipeline_initialization_.IsReady();
}

std::shared_ptr<Texture> ContentContext::GetEmptyTexture() const {
  return empty_texture_;
}

fml::StatusOr<RenderTarget> ContentContext::MakeSubpass(
    std::string_view label,
    ISize texture_size,
    const std::shared_ptr<CommandBuffer>& command_buffer,
    const SubpassCallback& subpass_callback,
    bool msaa_enabled,
    bool depth_stencil_enabled,
    int32_t mip_count,
    bool exact_texture_extent) const {
  const std::shared_ptr<Context>& context = GetContext();
  RenderTarget subpass_target;

  if (UsesAvioCoverage()) {
    if (!BeginAvioRasterFrame()) {
      return fml::Status(fml::StatusCode::kUnknown,
                         "Coverage frame initialization failed");
    }
    fml::ScopedCleanupClosure close_frame([this] { EndAvioRasterFrame(); });
    const auto allocation = GetAvioCoverageRegion()->AcquireLayer(
        texture_size, mip_count, exact_texture_extent);
    if (!allocation.lease) {
      return fml::Status(fml::StatusCode::kUnknown,
                         "Coverage layer allocation failed");
    }
    subpass_target = allocation.lease->GetRenderTarget();
    subpass_target.SetContentRect(allocation.lease->GetContentRect());
    subpass_target.SetResourceOwner(allocation.lease);
    // Clear the whole physical bank image. Filter taps outside the logical
    // rectangle must see transparent pixels rather than the previous tenant.
    return MakeSubpass(label, subpass_target, command_buffer, subpass_callback,
                       msaa_enabled || depth_stencil_enabled);
  }

  std::optional<RenderTarget::AttachmentConfig> depth_stencil_config =
      depth_stencil_enabled ? RenderTarget::kDefaultStencilAttachmentConfig
                            : std::optional<RenderTarget::AttachmentConfig>();

  if (context->GetCapabilities()->SupportsOffscreenMSAA() && msaa_enabled) {
    subpass_target = GetRenderTargetCache()->CreateOffscreenMSAA(
        /*context=*/*context,
        /*size=*/texture_size,
        /*mip_count=*/mip_count,
        /*label=*/label,
        /*color_attachment_config=*/
        RenderTarget::kDefaultColorAttachmentConfigMSAA,
        /*stencil_attachment_config=*/depth_stencil_config,
        /*existing_color_msaa_texture=*/nullptr,
        /*existing_color_resolve_texture=*/nullptr,
        /*existing_depth_stencil_texture=*/nullptr,
        /*target_pixel_format=*/std::nullopt);
  } else {
    subpass_target = GetRenderTargetCache()->CreateOffscreen(
        *context, texture_size,
        /*mip_count=*/mip_count, label,
        RenderTarget::kDefaultColorAttachmentConfig, depth_stencil_config);
  }
  return MakeSubpass(label, subpass_target, command_buffer, subpass_callback);
}

fml::StatusOr<RenderTarget> ContentContext::MakeSubpass(
    std::string_view label,
    const RenderTarget& subpass_target,
    const std::shared_ptr<CommandBuffer>& command_buffer,
    const SubpassCallback& subpass_callback,
    bool coverage_antialiasing) const {
  const std::shared_ptr<Context>& context = GetContext();

  auto subpass_texture = subpass_target.GetRenderTargetTexture();
  if (!subpass_texture) {
    return fml::Status(fml::StatusCode::kUnknown, "");
  }

  const bool coverage_parent =
      UsesAvioCoverage() &&
      subpass_target.GetSampleCount() == SampleCount::kCount1;
  if (UsesAvioCoverage() && !coverage_parent &&
      subpass_target.GetColorAttachment(0).texture !=
          GetAvioCoverageRegion()
              ->GetCoverageAtlasTarget()
              .GetColorAttachment(0)
              .texture) {
    return fml::Status(fml::StatusCode::kUnknown,
                       "Unbounded multisample coverage subpass rejected");
  }
  std::shared_ptr<RenderPass> sub_renderpass;
  if (coverage_parent) {
    sub_renderpass = coverage_antialiasing
                         ? CoverageTiledRenderPass::Make(*this, subpass_target,
                                                         command_buffer)
                         : CoverageTiledRenderPass::MakeDirect1x(
                               *this, subpass_target, command_buffer);
  } else {
    sub_renderpass = command_buffer->CreateRenderPass(subpass_target);
  }
  if (!sub_renderpass) {
    return fml::Status(fml::StatusCode::kUnknown, "");
  }
  sub_renderpass->SetLabel(label);

  if (!subpass_callback(*this, *sub_renderpass)) {
    return fml::Status(fml::StatusCode::kUnknown, "");
  }

  if (!sub_renderpass->EncodeCommands()) {
    return fml::Status(fml::StatusCode::kUnknown, "");
  }

  const std::shared_ptr<Texture>& target_texture =
      subpass_target.GetRenderTargetTexture();
  if (target_texture->GetMipCount() > 1) {
    fml::Status mipmap_status =
        AddMipmapGeneration(command_buffer, context, target_texture);
    if (!mipmap_status.ok()) {
      return mipmap_status;
    }
  }

  return subpass_target;
}

Tessellator& ContentContext::GetTessellator() const {
  return *tessellator_;
}

std::shared_ptr<Context> ContentContext::GetContext() const {
  return context_;
}

const Capabilities& ContentContext::GetDeviceCapabilities() const {
  return *context_->GetCapabilities();
}

PipelineRef ContentContext::GetCachedRuntimeEffectPipeline(
    const std::string& unique_entrypoint_name,
    const ContentContextOptions& options,
    const std::function<std::shared_ptr<Pipeline<PipelineDescriptor>>()>&
        create_callback) const {
  RuntimeEffectPipelineKey key{unique_entrypoint_name, options};
  auto it = runtime_effect_pipelines_.find(key);
  if (it == runtime_effect_pipelines_.end()) {
    it = runtime_effect_pipelines_.insert(it, {key, create_callback()});
  }
  return raw_ptr(it->second);
}

void ContentContext::ClearCachedRuntimeEffectPipeline(
    const std::string& unique_entrypoint_name) const {
#ifdef IMPELLER_DEBUG
  // destroying in-use pipleines is a validation error.
  const auto& idle_waiter = GetContext()->GetIdleWaiter();
  if (idle_waiter) {
    idle_waiter->WaitIdle();
  }
#endif  // IMPELLER_DEBUG
  for (auto it = runtime_effect_pipelines_.begin();
       it != runtime_effect_pipelines_.end();) {
    if (it->first.unique_entrypoint_name == unique_entrypoint_name) {
      it = runtime_effect_pipelines_.erase(it);
    } else {
      it++;
    }
  }
}

bool ContentContext::WarmAvioPipelineVariants(
    const std::shared_ptr<Pipeline<PipelineDescriptor>>& source) const {
  if (!source) {
    return false;
  }
  if (IsAvioRasterFrameActive() ||
      !context_->GetAvioAntialiasingConfig().UsesCoverage() ||
      context_->GetBackendType() != Context::BackendType::kVulkan) {
    return true;
  }
  std::scoped_lock lock(avio_pipeline_variants_mutex_);
  const size_t hash = (reinterpret_cast<uintptr_t>(source.get()) >> 4u) &
                      (kAvioPipelineVariantCapacity - 1u);
  for (size_t probe = 0; probe < kAvioPipelineVariantCapacity; ++probe) {
    auto& entry =
        avio_pipeline_variants_[(hash + probe) % kAvioPipelineVariantCapacity];
    if (entry.source == source) {
      return entry.ready;
    }
    if (entry.source) {
      continue;
    }
    entry.source = source;
    entry.ready = true;
    const auto fragment =
        source->GetDescriptor().GetEntrypointForStage(ShaderStage::kFragment);
    const auto name = fragment ? fragment->GetName() : std::string{};
    const auto* colour =
        source->GetDescriptor().GetColorAttachmentDescriptor(0);
    const bool preserves_colour =
        colour && colour->blending_enabled &&
        colour->src_color_blend_factor == BlendFactor::kZero &&
        colour->dst_color_blend_factor == BlendFactor::kOne &&
        colour->src_alpha_blend_factor == BlendFactor::kZero &&
        colour->dst_alpha_blend_factor == BlendFactor::kOne;
    const bool colour_output =
        colour && colour->write_mask != ColorWriteMaskBits::kNone &&
        colour->format != PixelFormat::kR8UNormInt && !preserves_colour;
    if (colour_output &&
        (name == "solid_fill_fragment_main" ||
         name == "texture_fill_fragment_main") &&
        source->GetDescriptor().GetSampleCount() == SampleCount::kCount4) {
      for (auto kind : {AvioCoveragePipelineVariant::kDirect1x,
                        AvioCoveragePipelineVariant::kJointSample4,
                        AvioCoveragePipelineVariant::kInteriorSample4,
                        AvioCoveragePipelineVariant::kFringeSample4}) {
        if (name == "texture_fill_fragment_main" &&
            kind == AvioCoveragePipelineVariant::kJointSample4) {
          continue;  // Raw sampled colour has not passed attachment rounding.
        }
        entry.variants[static_cast<size_t>(kind)] =
            CreateAvioCoveragePipelineVariant(*context_,
                                              source->GetDescriptor(), kind);
        entry.ready =
            entry.ready && entry.variants[static_cast<size_t>(kind)] != nullptr;
      }
    }
    if (colour_output && name == "texture_fill_fragment_main" &&
        source->GetDescriptor().GetSampleCount() == SampleCount::kCount1) {
      entry.variants[static_cast<size_t>(
          AvioCoveragePipelineVariant::kLayerSample4)] =
          CreateAvioCoveragePipelineVariant(
              *context_, source->GetDescriptor(),
              AvioCoveragePipelineVariant::kLayerSample4);
      entry.ready = entry.ready &&
                    entry.variants[static_cast<size_t>(
                        AvioCoveragePipelineVariant::kLayerSample4)] != nullptr;
    }
    if (colour_output &&
        source->GetDescriptor().GetSampleCount() == SampleCount::kCount4 &&
        context_->GetAvioAntialiasingConfig().continuous_requested_classes !=
            0) {
      entry.variants[static_cast<size_t>(
          AvioCoveragePipelineVariant::kContinuous)] =
          CreateAvioCoveragePipelineVariant(
              *context_, source->GetDescriptor(),
              AvioCoveragePipelineVariant::kContinuous);
      entry.ready = entry.ready &&
                    entry.variants[static_cast<size_t>(
                        AvioCoveragePipelineVariant::kContinuous)] != nullptr;
    }
    return entry.ready;
  }
  return false;
}

std::shared_ptr<Pipeline<PipelineDescriptor>>
ContentContext::GetAvioPipelineVariant(
    const std::shared_ptr<Pipeline<PipelineDescriptor>>& source,
    AvioCoveragePipelineVariant kind) const {
  if (!source || kind >= AvioCoveragePipelineVariant::kCount) {
    return nullptr;
  }
  std::unique_lock lock(avio_pipeline_variants_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    return nullptr;
  }
  const size_t hash = (reinterpret_cast<uintptr_t>(source.get()) >> 4u) &
                      (kAvioPipelineVariantCapacity - 1u);
  for (size_t probe = 0; probe < kAvioPipelineVariantCapacity; ++probe) {
    const auto& entry =
        avio_pipeline_variants_[(hash + probe) % kAvioPipelineVariantCapacity];
    if (entry.source == source) {
      return entry.variants[static_cast<size_t>(kind)];
    }
    if (!entry.source) {
      return nullptr;
    }
  }
  return nullptr;
}

void ContentContext::ResetTransientsBuffers() {
  if (continuous_data_host_buffer_) {
    continuous_data_host_buffer_->Reset();
  }
  data_host_buffer_->Reset();

  // We should only reset the indexes host buffer if it is actually different
  // from the data host buffer. Otherwise we'll end up resetting the same host
  // buffer twice.
  if (data_host_buffer_ != indexes_host_buffer_) {
    indexes_host_buffer_->Reset();
  }
}

void ContentContext::InitializeCommonlyUsedShadersIfNeeded() const {
  GetContext()->InitializeCommonlyUsedShadersIfNeeded();
}

bool ContentContext::PrewarmAvioCoveragePipelines() const {
  const auto region = context_->GetAvioCoverageRegion();
  if (!region) {
    return false;
  }
  const AvioPipelinePrewarmConfig config{
      .native_format = region->GetConfig().colour_format,
      .native_stencil_only = region->GetConfig().colour_depth_stencil_format ==
                             PixelFormat::kS8UInt,
      .supports_ssbo = GetDeviceCapabilities().SupportsSSBO(),
      .supports_triangle_fan = GetDeviceCapabilities().SupportsTriangleFan(),
      .use_sdfs = context_->GetFlags().use_sdfs,
      .continuous_sdf_cuts =
          context_->GetAvioAntialiasingConfig().RequestsContinuous(
              AvioContinuousClass::kArc) ||
          context_->GetAvioAntialiasingConfig().RequestsContinuous(
              AvioContinuousClass::kBorderedRoundedRect),
      .needs_gles_decal_downsample =
          context_->GetBackendType() == Context::BackendType::kOpenGLES &&
          !GetDeviceCapabilities().SupportsDecalSamplerAddressMode(),
  };
  using Family = AvioPrewarmPipeline;
  using Storage = AvioPrewarmGradientStorage;
  // One family selector owns both phases: queue every actual source key via
  // main's asynchronous default-descriptor authority, then join/validate the
  // same keys and their required Coverage derivatives before public Ready.
  const auto dispatch =
      [this](Family family, const ContentContextOptions& options,
             Storage storage, ConicalKind conical, bool queue) -> bool {
    const auto process = [&](auto& variants) {
      if (queue) {
        variants.Prewarm(*context_, options);
        return variants.Get(options) != nullptr;
      }
      return !!GetPipeline(this, variants, options);
    };
    const auto choose_conical = [&](auto& base, auto& radial, auto& strip,
                                    auto& strip_radial) {
      switch (conical) {
        case ConicalKind::kConical:
          return process(base);
        case ConicalKind::kRadial:
          return process(radial);
        case ConicalKind::kStrip:
          return process(strip);
        case ConicalKind::kStripAndRadial:
          return process(strip_radial);
      }
      return false;
    };
    switch (family) {
      case Family::kSolid:
        return process(pipelines_->solid_fill);
      case Family::kTexture:
        return process(pipelines_->texture);
      case Family::kStrictTexture:
        return process(pipelines_->texture_strict_src);
      case Family::kTiledTexture:
        return process(pipelines_->tiled_texture);
      case Family::kGlyph:
        return process(pipelines_->glyph_atlas);
      case Family::kShadowVertices:
        return process(pipelines_->shadow_vertices_);
      case Family::kCircle:
        return process(pipelines_->circle);
      case Family::kUberSdf:
        return GetDeviceCapabilities().SupportsSSBO()
                   ? process(pipelines_->uber_sdf_ssbo)
                   : process(pipelines_->uber_sdf);
      case Family::kComplexRse:
        return process(pipelines_->complex_rse);
      case Family::kFastGradient:
        return process(pipelines_->fast_gradient);
      case Family::kLinearGradient:
        switch (storage) {
          case Storage::kSsbo:
            return process(pipelines_->linear_gradient_ssbo_fill);
          case Storage::kUniform:
            return process(pipelines_->linear_gradient_uniform_fill);
          case Storage::kTexture:
            return process(pipelines_->linear_gradient_fill);
        }
        return false;
      case Family::kRadialGradient:
        switch (storage) {
          case Storage::kSsbo:
            return process(pipelines_->radial_gradient_ssbo_fill);
          case Storage::kUniform:
            return process(pipelines_->radial_gradient_uniform_fill);
          case Storage::kTexture:
            return process(pipelines_->radial_gradient_fill);
        }
        return false;
      case Family::kSweepGradient:
        switch (storage) {
          case Storage::kSsbo:
            return process(pipelines_->sweep_gradient_ssbo_fill);
          case Storage::kUniform:
            return process(pipelines_->sweep_gradient_uniform_fill);
          case Storage::kTexture:
            return process(pipelines_->sweep_gradient_fill);
        }
        return false;
      case Family::kConicalGradient:
        switch (storage) {
          case Storage::kSsbo:
            return choose_conical(
                pipelines_->conical_gradient_ssbo_fill,
                pipelines_->conical_gradient_ssbo_fill_radial,
                pipelines_->conical_gradient_ssbo_fill_strip,
                pipelines_->conical_gradient_ssbo_fill_strip_and_radial);
          case Storage::kUniform:
            return choose_conical(
                pipelines_->conical_gradient_uniform_fill,
                pipelines_->conical_gradient_uniform_fill_radial,
                pipelines_->conical_gradient_uniform_fill_strip,
                pipelines_->conical_gradient_uniform_fill_strip_and_radial);
          case Storage::kTexture:
            return choose_conical(
                pipelines_->conical_gradient_fill,
                pipelines_->conical_gradient_fill_radial,
                pipelines_->conical_gradient_fill_strip,
                pipelines_->conical_gradient_fill_strip_and_radial);
        }
        return false;
      case Family::kClip:
        return process(pipelines_->clip);
      case Family::kRRectBlur:
        return process(pipelines_->rrect_blur);
      case Family::kRSuperellipseBlur:
        return process(pipelines_->rsuperellipse_blur);
      case Family::kGaussianBlur:
        return process(pipelines_->gaussian_blur);
      case Family::kBorderMaskBlur:
        return process(pipelines_->border_mask_blur);
      case Family::kColorMatrix:
        return process(pipelines_->color_matrix_color_filter);
      case Family::kLinearToSrgb:
        return process(pipelines_->linear_to_srgb_filter);
      case Family::kSrgbToLinear:
        return process(pipelines_->srgb_to_linear_filter);
      case Family::kMorphology:
        return process(pipelines_->morphology_filter);
      case Family::kYuvToRgb:
        return process(pipelines_->yuv_to_rgb_filter);
      case Family::kDownsample:
        return process(pipelines_->texture_downsample);
      case Family::kDownsampleBounded:
        return process(pipelines_->texture_downsample_bounded);
      case Family::kDownsampleGles:
#ifdef IMPELLER_ENABLE_OPENGLES
        return process(pipelines_->texture_downsample_gles);
#else
        return false;
#endif
    }
    return false;
  };
  return PrewarmAvioPipelineKeysInTwoPhases(
      config,
      [&](Family family, const ContentContextOptions& options, Storage storage,
          ConicalKind conical) {
        return dispatch(family, options, storage, conical, true);
      },
      [&](Family family, const ContentContextOptions& options, Storage storage,
          ConicalKind conical) {
        return dispatch(family, options, storage, conical, false);
      });
}

void ContentContext::PrewarmPipelineVariant(const PrewarmVariant& variant) {
  const ContentContextOptions& options = variant.options;
  switch (variant.pipeline) {
    case PrewarmVariant::Pipeline::kUberSDF:
      pipelines_->uber_sdf.Prewarm(*context_, options);
      return;
    case PrewarmVariant::Pipeline::kUberSDFSSBO:
      pipelines_->uber_sdf_ssbo.Prewarm(*context_, options);
      return;
    case PrewarmVariant::Pipeline::kTexture:
      pipelines_->texture.Prewarm(*context_, options);
      return;
    case PrewarmVariant::Pipeline::kTiledTexture:
      pipelines_->tiled_texture.Prewarm(*context_, options);
      return;
    case PrewarmVariant::Pipeline::kFastGradient:
      pipelines_->fast_gradient.Prewarm(*context_, options);
      return;
    case PrewarmVariant::Pipeline::kLinearGradientSSBOFill:
      pipelines_->linear_gradient_ssbo_fill.Prewarm(*context_, options);
      return;
    case PrewarmVariant::Pipeline::kRadialGradientSSBOFill:
      pipelines_->radial_gradient_ssbo_fill.Prewarm(*context_, options);
      return;
  }
}

PipelineRef ContentContext::GetFastGradientPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->fast_gradient, opts);
}

PipelineRef ContentContext::GetLinearGradientFillPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->linear_gradient_fill, opts);
}

PipelineRef ContentContext::GetLinearGradientUniformFillPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->linear_gradient_uniform_fill, opts);
}

PipelineRef ContentContext::GetRadialGradientUniformFillPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->radial_gradient_uniform_fill, opts);
}

PipelineRef ContentContext::GetSweepGradientUniformFillPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->sweep_gradient_uniform_fill, opts);
}

PipelineRef ContentContext::GetLinearGradientSSBOFillPipeline(
    ContentContextOptions opts) const {
  FML_DCHECK(GetDeviceCapabilities().SupportsSSBO());
  return GetPipeline(this, pipelines_->linear_gradient_ssbo_fill, opts);
}

PipelineRef ContentContext::GetRadialGradientSSBOFillPipeline(
    ContentContextOptions opts) const {
  FML_DCHECK(GetDeviceCapabilities().SupportsSSBO());
  return GetPipeline(this, pipelines_->radial_gradient_ssbo_fill, opts);
}

PipelineRef ContentContext::GetConicalGradientUniformFillPipeline(
    ContentContextOptions opts,
    ConicalKind kind) const {
  switch (kind) {
    case ConicalKind::kConical:
      return GetPipeline(this, pipelines_->conical_gradient_uniform_fill, opts);
    case ConicalKind::kRadial:
      return GetPipeline(this, pipelines_->conical_gradient_uniform_fill_radial,
                         opts);
    case ConicalKind::kStrip:
      return GetPipeline(this, pipelines_->conical_gradient_uniform_fill_strip,
                         opts);
    case ConicalKind::kStripAndRadial:
      return GetPipeline(
          this, pipelines_->conical_gradient_uniform_fill_strip_and_radial,
          opts);
  }
}

PipelineRef ContentContext::GetConicalGradientSSBOFillPipeline(
    ContentContextOptions opts,
    ConicalKind kind) const {
  FML_DCHECK(GetDeviceCapabilities().SupportsSSBO());
  switch (kind) {
    case ConicalKind::kConical:
      return GetPipeline(this, pipelines_->conical_gradient_ssbo_fill, opts);
    case ConicalKind::kRadial:
      return GetPipeline(this, pipelines_->conical_gradient_ssbo_fill_radial,
                         opts);
    case ConicalKind::kStrip:
      return GetPipeline(this, pipelines_->conical_gradient_ssbo_fill_strip,
                         opts);
    case ConicalKind::kStripAndRadial:
      return GetPipeline(
          this, pipelines_->conical_gradient_ssbo_fill_strip_and_radial, opts);
  }
}

PipelineRef ContentContext::GetSweepGradientSSBOFillPipeline(
    ContentContextOptions opts) const {
  FML_DCHECK(GetDeviceCapabilities().SupportsSSBO());
  return GetPipeline(this, pipelines_->sweep_gradient_ssbo_fill, opts);
}

PipelineRef ContentContext::GetRadialGradientFillPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->radial_gradient_fill, opts);
}

PipelineRef ContentContext::GetConicalGradientFillPipeline(
    ContentContextOptions opts,
    ConicalKind kind) const {
  switch (kind) {
    case ConicalKind::kConical:
      return GetPipeline(this, pipelines_->conical_gradient_fill, opts);
    case ConicalKind::kRadial:
      return GetPipeline(this, pipelines_->conical_gradient_fill_radial, opts);
    case ConicalKind::kStrip:
      return GetPipeline(this, pipelines_->conical_gradient_fill_strip, opts);
    case ConicalKind::kStripAndRadial:
      return GetPipeline(
          this, pipelines_->conical_gradient_fill_strip_and_radial, opts);
  }
}

PipelineRef ContentContext::GetRRectBlurPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->rrect_blur, opts);
}

PipelineRef ContentContext::GetRSuperellipseBlurPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->rsuperellipse_blur, opts);
}

PipelineRef ContentContext::GetSweepGradientFillPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->sweep_gradient_fill, opts);
}

PipelineRef ContentContext::GetSolidFillPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->solid_fill, opts);
}

PipelineRef ContentContext::GetTexturePipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->texture, opts);
}

PipelineRef ContentContext::GetTextureStrictSrcPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->texture_strict_src, opts);
}

PipelineRef ContentContext::GetTiledTexturePipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->tiled_texture, opts);
}

PipelineRef ContentContext::GetGaussianBlurPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->gaussian_blur, opts);
}

PipelineRef ContentContext::GetBorderMaskBlurPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->border_mask_blur, opts);
}

PipelineRef ContentContext::GetMorphologyFilterPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->morphology_filter, opts);
}

PipelineRef ContentContext::GetColorMatrixColorFilterPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->color_matrix_color_filter, opts);
}

PipelineRef ContentContext::GetLinearToSrgbFilterPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->linear_to_srgb_filter, opts);
}

PipelineRef ContentContext::GetSrgbToLinearFilterPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->srgb_to_linear_filter, opts);
}

PipelineRef ContentContext::GetClipPipeline(ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->clip, opts);
}

PipelineRef ContentContext::GetCoverageMaskPipeline(
    ContentContextOptions opts) const {
#ifdef IMPELLER_ENABLE_VULKAN
  if (!UsesAvioCoverage() || opts.sample_count != SampleCount::kCount4) {
    return {};
  }
  return GetPipeline(this, pipelines_->coverage_mask, opts);
#else
  return {};
#endif
}

PipelineRef ContentContext::GetCoverageQuadPipeline(
    ContentContextOptions opts) const {
#ifdef IMPELLER_ENABLE_VULKAN
  if (!UsesAvioCoverage() || opts.sample_count != SampleCount::kCount4) {
    return {};
  }
  return GetPipeline(this, pipelines_->coverage_quad, opts);
#else
  return {};
#endif
}

CoveragePathAtlas* ContentContext::GetCoveragePathAtlas() const {
  return coverage_path_atlas_.get();
}

PipelineRef ContentContext::GetGlyphAtlasPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->glyph_atlas, opts);
}

PipelineRef ContentContext::GetYUVToRGBFilterPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->yuv_to_rgb_filter, opts);
}

PipelineRef ContentContext::GetUberSDFPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->uber_sdf, opts);
}

PipelineRef ContentContext::GetUberSDFSSBOPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->uber_sdf_ssbo, opts);
}

PipelineRef ContentContext::GetComplexRSEPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->complex_rse, opts);
}

PipelineRef ContentContext::GetPorterDuffPipeline(
    BlendMode mode,
    ContentContextOptions opts) const {
  switch (mode) {
    case BlendMode::kClear:
      return GetClearBlendPipeline(opts);
    case BlendMode::kSrc:
      return GetSourceBlendPipeline(opts);
    case BlendMode::kDst:
      return GetDestinationBlendPipeline(opts);
    case BlendMode::kSrcOver:
      return GetSourceOverBlendPipeline(opts);
    case BlendMode::kDstOver:
      return GetDestinationOverBlendPipeline(opts);
    case BlendMode::kSrcIn:
      return GetSourceInBlendPipeline(opts);
    case BlendMode::kDstIn:
      return GetDestinationInBlendPipeline(opts);
    case BlendMode::kSrcOut:
      return GetSourceOutBlendPipeline(opts);
    case BlendMode::kDstOut:
      return GetDestinationOutBlendPipeline(opts);
    case BlendMode::kSrcATop:
      return GetSourceATopBlendPipeline(opts);
    case BlendMode::kDstATop:
      return GetDestinationATopBlendPipeline(opts);
    case BlendMode::kXor:
      return GetXorBlendPipeline(opts);
    case BlendMode::kPlus:
      return GetPlusBlendPipeline(opts);
    case BlendMode::kModulate:
      return GetModulateBlendPipeline(opts);
    case BlendMode::kScreen:
      return GetScreenBlendPipeline(opts);
    case BlendMode::kOverlay:
    case BlendMode::kDarken:
    case BlendMode::kLighten:
    case BlendMode::kColorDodge:
    case BlendMode::kColorBurn:
    case BlendMode::kHardLight:
    case BlendMode::kSoftLight:
    case BlendMode::kDifference:
    case BlendMode::kExclusion:
    case BlendMode::kMultiply:
    case BlendMode::kHue:
    case BlendMode::kSaturation:
    case BlendMode::kColor:
    case BlendMode::kLuminosity:
      VALIDATION_LOG << "Invalid porter duff blend mode "
                     << BlendModeToString(mode);
      return GetClearBlendPipeline(opts);
      break;
  }
}

PipelineRef ContentContext::GetClearBlendPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->clear_blend, opts);
}

PipelineRef ContentContext::GetSourceBlendPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->source_blend, opts);
}

PipelineRef ContentContext::GetDestinationBlendPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->destination_blend, opts);
}

PipelineRef ContentContext::GetSourceOverBlendPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->source_over_blend, opts);
}

PipelineRef ContentContext::GetDestinationOverBlendPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->destination_over_blend, opts);
}

PipelineRef ContentContext::GetSourceInBlendPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->source_in_blend, opts);
}

PipelineRef ContentContext::GetDestinationInBlendPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->destination_in_blend, opts);
}

PipelineRef ContentContext::GetSourceOutBlendPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->source_out_blend, opts);
}

PipelineRef ContentContext::GetDestinationOutBlendPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->destination_out_blend, opts);
}

PipelineRef ContentContext::GetSourceATopBlendPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->source_a_top_blend, opts);
}

PipelineRef ContentContext::GetDestinationATopBlendPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->destination_a_top_blend, opts);
}

PipelineRef ContentContext::GetXorBlendPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->xor_blend, opts);
}

PipelineRef ContentContext::GetPlusBlendPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->plus_blend, opts);
}

PipelineRef ContentContext::GetModulateBlendPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->modulate_blend, opts);
}

PipelineRef ContentContext::GetScreenBlendPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->screen_blend, opts);
}

PipelineRef ContentContext::GetBlendColorPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->blend_color, opts);
}

PipelineRef ContentContext::GetBlendColorBurnPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->blend_colorburn, opts);
}

PipelineRef ContentContext::GetBlendColorDodgePipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->blend_colordodge, opts);
}

PipelineRef ContentContext::GetBlendDarkenPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->blend_darken, opts);
}

PipelineRef ContentContext::GetBlendDifferencePipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->blend_difference, opts);
}

PipelineRef ContentContext::GetBlendExclusionPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->blend_exclusion, opts);
}

PipelineRef ContentContext::GetBlendHardLightPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->blend_hardlight, opts);
}

PipelineRef ContentContext::GetBlendHuePipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->blend_hue, opts);
}

PipelineRef ContentContext::GetBlendLightenPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->blend_lighten, opts);
}

PipelineRef ContentContext::GetBlendLuminosityPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->blend_luminosity, opts);
}

PipelineRef ContentContext::GetBlendMultiplyPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->blend_multiply, opts);
}

PipelineRef ContentContext::GetBlendOverlayPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->blend_overlay, opts);
}

PipelineRef ContentContext::GetBlendSaturationPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->blend_saturation, opts);
}

PipelineRef ContentContext::GetBlendScreenPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->blend_screen, opts);
}

PipelineRef ContentContext::GetBlendSoftLightPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->blend_softlight, opts);
}

PipelineRef ContentContext::GetDownsamplePipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->texture_downsample, opts);
}

PipelineRef ContentContext::GetDownsampleBoundedPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->texture_downsample_bounded, opts);
}

PipelineRef ContentContext::GetFramebufferBlendColorPipeline(
    ContentContextOptions opts) const {
  FML_DCHECK(GetDeviceCapabilities().SupportsFramebufferFetch());
  return GetPipeline(this, pipelines_->framebuffer_blend_color, opts);
}

PipelineRef ContentContext::GetFramebufferBlendColorBurnPipeline(
    ContentContextOptions opts) const {
  FML_DCHECK(GetDeviceCapabilities().SupportsFramebufferFetch());
  return GetPipeline(this, pipelines_->framebuffer_blend_colorburn, opts);
}

PipelineRef ContentContext::GetFramebufferBlendColorDodgePipeline(
    ContentContextOptions opts) const {
  FML_DCHECK(GetDeviceCapabilities().SupportsFramebufferFetch());
  return GetPipeline(this, pipelines_->framebuffer_blend_colordodge, opts);
}

PipelineRef ContentContext::GetFramebufferBlendDarkenPipeline(
    ContentContextOptions opts) const {
  FML_DCHECK(GetDeviceCapabilities().SupportsFramebufferFetch());
  return GetPipeline(this, pipelines_->framebuffer_blend_darken, opts);
}

PipelineRef ContentContext::GetFramebufferBlendDifferencePipeline(
    ContentContextOptions opts) const {
  FML_DCHECK(GetDeviceCapabilities().SupportsFramebufferFetch());
  return GetPipeline(this, pipelines_->framebuffer_blend_difference, opts);
}

PipelineRef ContentContext::GetFramebufferBlendExclusionPipeline(
    ContentContextOptions opts) const {
  FML_DCHECK(GetDeviceCapabilities().SupportsFramebufferFetch());
  return GetPipeline(this, pipelines_->framebuffer_blend_exclusion, opts);
}

PipelineRef ContentContext::GetFramebufferBlendHardLightPipeline(
    ContentContextOptions opts) const {
  FML_DCHECK(GetDeviceCapabilities().SupportsFramebufferFetch());
  return GetPipeline(this, pipelines_->framebuffer_blend_hardlight, opts);
}

PipelineRef ContentContext::GetFramebufferBlendHuePipeline(
    ContentContextOptions opts) const {
  FML_DCHECK(GetDeviceCapabilities().SupportsFramebufferFetch());
  return GetPipeline(this, pipelines_->framebuffer_blend_hue, opts);
}

PipelineRef ContentContext::GetFramebufferBlendLightenPipeline(
    ContentContextOptions opts) const {
  FML_DCHECK(GetDeviceCapabilities().SupportsFramebufferFetch());
  return GetPipeline(this, pipelines_->framebuffer_blend_lighten, opts);
}

PipelineRef ContentContext::GetFramebufferBlendLuminosityPipeline(
    ContentContextOptions opts) const {
  FML_DCHECK(GetDeviceCapabilities().SupportsFramebufferFetch());
  return GetPipeline(this, pipelines_->framebuffer_blend_luminosity, opts);
}

PipelineRef ContentContext::GetFramebufferBlendMultiplyPipeline(
    ContentContextOptions opts) const {
  FML_DCHECK(GetDeviceCapabilities().SupportsFramebufferFetch());
  return GetPipeline(this, pipelines_->framebuffer_blend_multiply, opts);
}

PipelineRef ContentContext::GetFramebufferBlendOverlayPipeline(
    ContentContextOptions opts) const {
  FML_DCHECK(GetDeviceCapabilities().SupportsFramebufferFetch());
  return GetPipeline(this, pipelines_->framebuffer_blend_overlay, opts);
}

PipelineRef ContentContext::GetFramebufferBlendSaturationPipeline(
    ContentContextOptions opts) const {
  FML_DCHECK(GetDeviceCapabilities().SupportsFramebufferFetch());
  return GetPipeline(this, pipelines_->framebuffer_blend_saturation, opts);
}

PipelineRef ContentContext::GetFramebufferBlendScreenPipeline(
    ContentContextOptions opts) const {
  FML_DCHECK(GetDeviceCapabilities().SupportsFramebufferFetch());
  return GetPipeline(this, pipelines_->framebuffer_blend_screen, opts);
}

PipelineRef ContentContext::GetFramebufferBlendSoftLightPipeline(
    ContentContextOptions opts) const {
  FML_DCHECK(GetDeviceCapabilities().SupportsFramebufferFetch());
  return GetPipeline(this, pipelines_->framebuffer_blend_softlight, opts);
}

PipelineRef ContentContext::GetDrawShadowVerticesPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->shadow_vertices_, opts);
}

PipelineRef ContentContext::GetDrawVerticesUberPipeline(
    BlendMode blend_mode,
    ContentContextOptions opts) const {
  if (blend_mode <= BlendMode::kHardLight) {
    return GetPipeline(this, pipelines_->vertices_uber_1_, opts);
  } else {
    return GetPipeline(this, pipelines_->vertices_uber_2_, opts);
  }
}

PipelineRef ContentContext::GetCirclePipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->circle, opts);
}

#ifdef IMPELLER_ENABLE_OPENGLES

#if !defined(FML_OS_EMSCRIPTEN)
PipelineRef ContentContext::GetTiledTextureUvExternalPipeline(
    ContentContextOptions opts) const {
  FML_DCHECK(GetContext()->GetBackendType() == Context::BackendType::kOpenGLES);
  return GetPipeline(this, pipelines_->tiled_texture_uv_external, opts);
}

PipelineRef ContentContext::GetTiledTextureExternalPipeline(
    ContentContextOptions opts) const {
  FML_DCHECK(GetContext()->GetBackendType() == Context::BackendType::kOpenGLES);
  return GetPipeline(this, pipelines_->tiled_texture_external, opts);
}
#endif

PipelineRef ContentContext::GetDownsampleTextureGlesPipeline(
    ContentContextOptions opts) const {
  return GetPipeline(this, pipelines_->texture_downsample_gles, opts);
}

#endif  // IMPELLER_ENABLE_OPENGLES

void ContentContext::SetTextureCachingEnabled(bool enabled) {
  is_texture_caching_enabled_ = enabled;
  if (!enabled) {
    texture_cache_.clear();
  }
}

std::shared_ptr<Texture> ContentContext::GetCachedTexture(
    const flutter::DlImage* image) const {
  if (!image) {
    return nullptr;
  }
  if (is_texture_caching_enabled_) {
    auto it = texture_cache_.find(image);
    if (it != texture_cache_.end()) {
      return it->second;
    }
  }
  return nullptr;
}

void ContentContext::SetCachedTexture(
    const flutter::DlImage* image,
    const std::shared_ptr<Texture>& texture) const {
  if (!image || !texture) {
    return;
  }
  if (is_texture_caching_enabled_) {
    texture_cache_[image] = texture;
  }
}

void ContentContext::RemoveCachedTexture(const flutter::DlImage* image) const {
  texture_cache_.erase(image);
}

void ContentContext::ClearCachedTextures() const {
  texture_cache_.clear();
}

bool ContentContext::UsesAvioCoverage() const {
  return context_->GetAvioAntialiasingConfig().UsesCoverage();
}

std::shared_ptr<AvioCoverageRegion> ContentContext::GetAvioCoverageRegion()
    const {
  return context_->GetAvioCoverageRegion();
}

bool ContentContext::BeginAvioRasterFrame() const {
  if (!UsesAvioCoverage()) {
    return true;
  }
  if (avio_raster_frame_depth_ != 0u) {
    ++avio_raster_frame_depth_;
    return true;
  }
  ReclaimUnusedAvioSample4Clips();
  static std::atomic<uint64_t> next_epoch{1u};
  const auto region = GetAvioCoverageRegion();
  if (!region || !region->BeginRasterFrame(
                     next_epoch.fetch_add(1u, std::memory_order_relaxed))) {
    return false;
  }
  avio_raster_frame_depth_ = 1u;
  return true;
}

void ContentContext::EndAvioRasterFrame() const {
  if (!UsesAvioCoverage() || avio_raster_frame_depth_ == 0u) {
    return;
  }
  if (--avio_raster_frame_depth_ == 0u) {
    GetAvioCoverageRegion()->EndRasterFrame();
  }
}

AvioRenderResourceReport ContentContext::GetAvioRenderResourceReport(
    bool start_new_interval) const {
  AvioRenderResourceReport report;
  report.available = IsValid();
  if (!report.available) {
    return report;
  }
  // Vulkan's physical allocation ledger accounts every image once, including
  // cached targets retained by in-flight submissions. Do not add cache bytes
  // to that census a second time.
  if (context_->GetBackendType() != Context::BackendType::kVulkan) {
    AvioRenderResourceEntry cache;
    cache.kind_id = static_cast<uint32_t>(AvioRenderResourceKind::kOffscreens);
    cache.usage = GetRenderTargetCache()->ReportUsage(start_new_interval);
    cache.fields_supported =
        kAvioResourceFieldCounts | kAvioResourceFieldDescriptorBytes;
    cache.unsupported_reason_id =
        kAvioResourceReasonPhysicalAllocationUnavailable;
    report.AddEntry(cache);
  }
  if (const auto region = GetAvioCoverageRegion()) {
    auto usage = region->ReportUsage(start_new_interval);
    report.counters_supported |= kAvioCounterCoverageFlushes;
    if (region->HasExactAllocatedBytes()) {
      report.counters_supported |= kAvioCounterLayerRegionOverflows;
    }
    report.coverage_flushes = usage.coverage_flushes;
    report.layer_region_overflows = usage.layer_region_overflow;
    report.layer_region_overflow_real_bytes =
        usage.layer_region_overflow_real_bytes;
  }
  return report;
}

}  // namespace impeller
