// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_CONTENT_CONTEXT_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_CONTENT_CONTEXT_H_

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "flutter/display_list/image/dl_image.h"
#include "flutter/fml/logging.h"
#include "flutter/fml/status_or.h"
#include "impeller/base/validation.h"
#include "impeller/core/formats.h"
#include "impeller/core/host_buffer.h"
#include "impeller/entity/contents/avio_pipeline_initialization.h"
#include "impeller/entity/contents/content_context_options.h"
#include "impeller/entity/contents/continuous_clip_pool.h"
#include "impeller/entity/contents/sample4_clip.h"
#include "impeller/entity/contents/sample4_clip_pipeline.h"
#include "impeller/entity/contents/text_shadow_cache.h"
#include "impeller/geometry/color.h"
#include "impeller/renderer/capabilities.h"
#include "impeller/renderer/command_buffer.h"
#include "impeller/renderer/pipeline.h"
#include "impeller/renderer/pipeline_descriptor.h"
#include "impeller/renderer/render_target.h"
#include "impeller/typographer/lazy_glyph_atlas.h"
#include "impeller/typographer/typographer_context.h"

namespace impeller {

/// A pipeline variant that `ContentContext` compiles asynchronously when it is
/// constructed, so that its first use does not compile it synchronously on the
/// raster thread. Every draw computes options that differ from its pipeline's
/// default (the default compares depth with `kAlways`; `OptionsFromPass`
/// selects `kGreaterEqual`), so without this the first use of each variant
/// compiles it on the calling thread.
struct PrewarmVariant {
  enum class Pipeline : uint8_t {
    kUberSDF,
    kUberSDFSSBO,
    kTexture,
    kTiledTexture,
    kFastGradient,
    kLinearGradientSSBOFill,
    kRadialGradientSSBOFill,
  };

  Pipeline pipeline = Pipeline::kTexture;
  ContentContextOptions options;

  bool operator==(const PrewarmVariant& other) const {
    return pipeline == other.pipeline &&
           options.ToKey() == other.options.ToKey();
  }
};

/// The render targets whose pipeline variants are prewarmed.
struct PrewarmTargets {
  /// Save layers, snapshots and filter targets
  /// (`Capabilities::GetDefaultColorFormat`).
  PixelFormat offscreen_format = PixelFormat::kUnknown;
  /// The embedder's root targets.
  PixelFormat root_format = PixelFormat::kUnknown;
  /// Whether save layers and the root pass are multisampled
  /// (`Capabilities::SupportsOffscreenMSAA`).
  bool multisampled_passes = false;
  /// Whether UberSDF and gradients read their stops from a storage buffer
  /// (`Capabilities::SupportsSSBO`).
  bool supports_ssbo = false;
};

/// The targets Avio renders into on a context with `capabilities`: its
/// offscreens, and roots that are DRM ARGB8888 images (`kB8G8R8A8UNormInt`).
PrewarmTargets MakeAvioPrewarmTargets(const Capabilities& capabilities);

/// The pipeline variants that Avio's UberSDF (patch 50), SDF color source
/// (patch 52) and backdrop Flip (patch 48) paths draw with into `targets`.
/// Contexts that render with SDFs compile these at construction.
std::vector<PrewarmVariant> MakeAvioPrewarmVariants(
    const PrewarmTargets& targets);

class Tessellator;
class RenderTargetCache;
class AvioCoverageRegion;
class CoverageRecorderStorage;
class CoveragePathAtlas;
class CoverageDisplayListPlan;

class ContentContext {
 public:
  explicit ContentContext(
      std::shared_ptr<Context> context,
      std::shared_ptr<TypographerContext> typographer_context,
      std::shared_ptr<RenderTargetAllocator> render_target_allocator = nullptr);

  ~ContentContext();

  bool IsValid() const;

  // Cold factory access only: public IsValid remains false until every
  // negotiated pipeline and storage resource has finished initialization.
  bool CanCreatePipelines() const {
    return pipeline_initialization_.CanCreatePipelines();
  }
  bool UsesAvioCoverage() const;
  std::shared_ptr<AvioCoverageRegion> GetAvioCoverageRegion() const;
  std::shared_ptr<CoverageRecorderStorage> GetCoverageRecorderStorage() const {
    return coverage_recorder_storage_;
  }
  CoveragePathAtlas* GetCoveragePathAtlas() const;
  AvioRenderResourceReport GetAvioRenderResourceReport(
      bool start_new_interval) const;

  // Nested snapshots share the enclosing raster frame's fixed regions. The
  // final scope closes only after all recorded readers have been encoded.
  bool BeginAvioRasterFrame() const;
  void EndAvioRasterFrame() const;

  std::shared_ptr<AvioContinuousClipExpression>
  AcquireAvioContinuousClipExpression(
      const std::shared_ptr<const AvioContinuousClipExpression>& previous)
      const {
    return continuous_clip_pool_ ? continuous_clip_pool_->Acquire(previous)
                                 : nullptr;
  }

  std::shared_ptr<AvioSample4ClipDescriptor> AcquireAvioSample4Clip(
      const std::shared_ptr<const AvioSample4ClipDescriptor>& previous) const {
    return sample4_clip_pool_ ? sample4_clip_pool_->Acquire(previous) : nullptr;
  }

  void ReclaimUnusedAvioSample4Clips() const {
    if (sample4_clip_pool_) {
      sample4_clip_pool_->ReclaimUnused();
    }
  }

  Tessellator& GetTessellator() const;

  // clang-format off
  PipelineRef GetBlendColorBurnPipeline(ContentContextOptions opts) const;
  PipelineRef GetBlendColorDodgePipeline(ContentContextOptions opts) const;
  PipelineRef GetBlendColorPipeline(ContentContextOptions opts) const;
  PipelineRef GetBlendDarkenPipeline(ContentContextOptions opts) const;
  PipelineRef GetBlendDifferencePipeline(ContentContextOptions opts) const;
  PipelineRef GetBlendExclusionPipeline(ContentContextOptions opts) const;
  PipelineRef GetBlendHardLightPipeline(ContentContextOptions opts) const;
  PipelineRef GetBlendHuePipeline(ContentContextOptions opts) const;
  PipelineRef GetBlendLightenPipeline(ContentContextOptions opts) const;
  PipelineRef GetBlendLuminosityPipeline(ContentContextOptions opts) const;
  PipelineRef GetBlendMultiplyPipeline(ContentContextOptions opts) const;
  PipelineRef GetBlendOverlayPipeline(ContentContextOptions opts) const;
  PipelineRef GetBlendSaturationPipeline(ContentContextOptions opts) const;
  PipelineRef GetBlendScreenPipeline(ContentContextOptions opts) const;
  PipelineRef GetBlendSoftLightPipeline(ContentContextOptions opts) const;
  PipelineRef GetBorderMaskBlurPipeline(ContentContextOptions opts) const;
  PipelineRef GetCirclePipeline(ContentContextOptions opts) const;
  PipelineRef GetClearBlendPipeline(ContentContextOptions opts) const;
  PipelineRef GetClipPipeline(ContentContextOptions opts) const;
  PipelineRef GetCoverageMaskPipeline(ContentContextOptions opts) const;
  PipelineRef GetCoverageQuadPipeline(ContentContextOptions opts) const;
  PipelineRef GetColorMatrixColorFilterPipeline(ContentContextOptions opts) const;
  PipelineRef GetConicalGradientFillPipeline(ContentContextOptions opts, ConicalKind kind) const;
  PipelineRef GetConicalGradientSSBOFillPipeline(ContentContextOptions opts, ConicalKind kind) const;
  PipelineRef GetConicalGradientUniformFillPipeline(ContentContextOptions opts, ConicalKind kind) const;
  PipelineRef GetDestinationATopBlendPipeline(ContentContextOptions opts) const;
  PipelineRef GetDestinationBlendPipeline(ContentContextOptions opts) const;
  PipelineRef GetDestinationInBlendPipeline(ContentContextOptions opts) const;
  PipelineRef GetDestinationOutBlendPipeline(ContentContextOptions opts) const;
  PipelineRef GetDestinationOverBlendPipeline(ContentContextOptions opts) const;
  PipelineRef GetDownsamplePipeline(ContentContextOptions opts) const;
  PipelineRef GetDrawShadowVerticesPipeline(ContentContextOptions opts) const;
  PipelineRef GetDownsampleBoundedPipeline(ContentContextOptions opts) const;
  PipelineRef GetDrawVerticesUberPipeline(BlendMode blend_mode, ContentContextOptions opts) const;
  PipelineRef GetFastGradientPipeline(ContentContextOptions opts) const;
  PipelineRef GetFramebufferBlendColorBurnPipeline(ContentContextOptions opts) const;
  PipelineRef GetFramebufferBlendColorDodgePipeline(ContentContextOptions opts) const;
  PipelineRef GetFramebufferBlendColorPipeline( ContentContextOptions opts) const;
  PipelineRef GetFramebufferBlendDarkenPipeline(ContentContextOptions opts) const;
  PipelineRef GetFramebufferBlendDifferencePipeline(ContentContextOptions opts) const;
  PipelineRef GetFramebufferBlendExclusionPipeline(ContentContextOptions opts) const;
  PipelineRef GetFramebufferBlendHardLightPipeline(ContentContextOptions opts) const;
  PipelineRef GetFramebufferBlendHuePipeline(ContentContextOptions opts) const;
  PipelineRef GetFramebufferBlendLightenPipeline(ContentContextOptions opts) const;
  PipelineRef GetFramebufferBlendLuminosityPipeline(ContentContextOptions opts) const;
  PipelineRef GetFramebufferBlendMultiplyPipeline(ContentContextOptions opts) const;
  PipelineRef GetFramebufferBlendOverlayPipeline(ContentContextOptions opts) const;
  PipelineRef GetFramebufferBlendSaturationPipeline(ContentContextOptions opts) const;
  PipelineRef GetFramebufferBlendScreenPipeline(ContentContextOptions opts) const;
  PipelineRef GetFramebufferBlendSoftLightPipeline(ContentContextOptions opts) const;
  PipelineRef GetGaussianBlurPipeline(ContentContextOptions opts) const;
  PipelineRef GetGlyphAtlasPipeline(ContentContextOptions opts) const;
  PipelineRef GetLinearGradientFillPipeline(ContentContextOptions opts) const;
  PipelineRef GetLinearGradientSSBOFillPipeline(ContentContextOptions opts) const;
  PipelineRef GetLinearGradientUniformFillPipeline(ContentContextOptions opts) const;
  PipelineRef GetLinearToSrgbFilterPipeline(ContentContextOptions opts) const;
  PipelineRef GetModulateBlendPipeline(ContentContextOptions opts) const;
  PipelineRef GetMorphologyFilterPipeline(ContentContextOptions opts) const;
  PipelineRef GetPlusBlendPipeline(ContentContextOptions opts) const;
  PipelineRef GetPorterDuffPipeline(BlendMode mode, ContentContextOptions opts) const;
  PipelineRef GetRadialGradientFillPipeline(ContentContextOptions opts) const;
  PipelineRef GetRadialGradientSSBOFillPipeline(ContentContextOptions opts) const;
  PipelineRef GetRadialGradientUniformFillPipeline(ContentContextOptions opts) const;
  PipelineRef GetRRectBlurPipeline(ContentContextOptions opts) const;
  PipelineRef GetRSuperellipseBlurPipeline(ContentContextOptions opts) const;
  PipelineRef GetScreenBlendPipeline(ContentContextOptions opts) const;
  PipelineRef GetSolidFillPipeline(ContentContextOptions opts) const;
  PipelineRef GetSourceATopBlendPipeline(ContentContextOptions opts) const;
  PipelineRef GetSourceBlendPipeline(ContentContextOptions opts) const;
  PipelineRef GetSourceInBlendPipeline(ContentContextOptions opts) const;
  PipelineRef GetSourceOutBlendPipeline(ContentContextOptions opts) const;
  PipelineRef GetSourceOverBlendPipeline(ContentContextOptions opts) const;
  PipelineRef GetSrgbToLinearFilterPipeline(ContentContextOptions opts) const;
  PipelineRef GetSweepGradientFillPipeline(ContentContextOptions opts) const;
  PipelineRef GetSweepGradientSSBOFillPipeline( ContentContextOptions opts) const;
  PipelineRef GetSweepGradientUniformFillPipeline(ContentContextOptions opts) const;
  PipelineRef GetTexturePipeline(ContentContextOptions opts) const;
  PipelineRef GetTextureStrictSrcPipeline(ContentContextOptions opts) const;
  PipelineRef GetTiledTexturePipeline(ContentContextOptions opts) const;
  PipelineRef GetXorBlendPipeline(ContentContextOptions opts) const;
  PipelineRef GetYUVToRGBFilterPipeline(ContentContextOptions opts) const;
  PipelineRef GetUberSDFPipeline(ContentContextOptions opts) const;
  PipelineRef GetUberSDFSSBOPipeline(ContentContextOptions opts) const;
  PipelineRef GetComplexRSEPipeline(ContentContextOptions opts) const;
#ifdef IMPELLER_ENABLE_OPENGLES
#if !defined(FML_OS_EMSCRIPTEN)
  PipelineRef GetTiledTextureExternalPipeline(ContentContextOptions opts) const;
  PipelineRef GetTiledTextureUvExternalPipeline(ContentContextOptions opts) const;
#endif
  PipelineRef GetDownsampleTextureGlesPipeline(ContentContextOptions opts) const;
#endif  // IMPELLER_ENABLE_OPENGLES
  // clang-format on

  // An empty 1x1 texture for binding drawVertices/drawAtlas or other cases
  // that don't always have a texture (due to blending).
  std::shared_ptr<Texture> GetEmptyTexture() const;

  std::shared_ptr<Context> GetContext() const;

  // Populated by the display-list owner at cold AiksContext initialization.
  // Forward declaration keeps entity independent of the dispatch library.
  void SetCoverageClassifierStorage(
      std::shared_ptr<CoverageDisplayListPlan> storage) {
    coverage_classifier_storage_ = std::move(storage);
  }
  std::shared_ptr<CoverageDisplayListPlan> GetCoverageClassifierStorage()
      const {
    return coverage_classifier_storage_;
  }
  // Census access that leaves the plan's exclusive-owner use count intact.
  const CoverageDisplayListPlan* GetCoverageClassifierPlan() const {
    return coverage_classifier_storage_.get();
  }

  const Capabilities& GetDeviceCapabilities() const;

  using SubpassCallback =
      std::function<bool(const ContentContext&, RenderPass&)>;

  /// @brief  Creates a new texture of size `texture_size` and calls
  ///         `subpass_callback` with a `RenderPass` for drawing to the texture.
  fml::StatusOr<RenderTarget> MakeSubpass(
      std::string_view label,
      ISize texture_size,
      const std::shared_ptr<CommandBuffer>& command_buffer,
      const SubpassCallback& subpass_callback,
      bool msaa_enabled = true,
      bool depth_stencil_enabled = false,
      int32_t mip_count = 1,
      bool exact_texture_extent = false) const;

  /// Makes a subpass that will render to `subpass_target`.
  fml::StatusOr<RenderTarget> MakeSubpass(
      std::string_view label,
      const RenderTarget& subpass_target,
      const std::shared_ptr<CommandBuffer>& command_buffer,
      const SubpassCallback& subpass_callback,
      bool coverage_antialiasing = false) const;

  const std::shared_ptr<LazyGlyphAtlas>& GetLazyGlyphAtlas() const {
    return lazy_glyph_atlas_;
  }

  const std::shared_ptr<RenderTargetAllocator>& GetRenderTargetCache() const {
    return render_target_cache_;
  }

  /// RuntimeEffect pipelines must be obtained via this method to avoid
  /// re-creating them every frame.
  ///
  /// The unique_entrypoint_name comes from RuntimeEffect::GetEntrypoint.
  /// Impellerc generates a unique entrypoint name for runtime effect shaders
  /// based on the input file name and shader stage.
  ///
  /// The create_callback is synchronously invoked exactly once if a cached
  /// pipeline is not found.
  PipelineRef GetCachedRuntimeEffectPipeline(
      const std::string& unique_entrypoint_name,
      const ContentContextOptions& options,
      const std::function<std::shared_ptr<Pipeline<PipelineDescriptor>>()>&
          create_callback) const;

  /// Used by hot reload/hot restart to clear a cached pipeline from
  /// GetCachedRuntimeEffectPipeline.
  void ClearCachedRuntimeEffectPipeline(
      const std::string& unique_entrypoint_name) const;

  /// @brief Enable or disable texture caching.
  void SetTextureCachingEnabled(bool enabled);

  /// @brief Get a cached texture for the given image.
  std::shared_ptr<Texture> GetCachedTexture(
      const flutter::DlImage* image) const;

  /// @brief Set a cached texture for the given image.
  void SetCachedTexture(const flutter::DlImage* image,
                        const std::shared_ptr<Texture>& texture) const;

  /// @brief Remove a cached texture for the given image.
  void RemoveCachedTexture(const flutter::DlImage* image) const;

  /// @brief Clear all cached textures.
  void ClearCachedTextures() const;

  /// @brief Retrieve the current host buffer for transient storage of indexes
  ///        used for indexed draws.
  ///
  /// This may or may not return the same value as `GetTransientsDataBuffer`
  /// depending on the backend.
  ///
  /// This is only safe to use from the raster threads. Other threads should
  /// allocate their own device buffers.
  HostBuffer& GetTransientsIndexesBuffer() const {
    return *indexes_host_buffer_;
  }

  /// @brief Retrieve the current host buffer for transient storage of other
  ///        non-index data.
  ///
  /// This is only safe to use from the raster threads. Other threads should
  /// allocate their own device buffers.
  HostBuffer& GetTransientsDataBuffer() const { return *data_host_buffer_; }
  bool WarmAvioPipelineVariants(
      const std::shared_ptr<Pipeline<PipelineDescriptor>>& source) const;
  std::shared_ptr<Pipeline<PipelineDescriptor>> GetAvioPipelineVariant(
      const std::shared_ptr<Pipeline<PipelineDescriptor>>& source,
      AvioCoveragePipelineVariant kind) const;

  HostBuffer* GetAvioContinuousDataBuffer() const {
    return continuous_data_host_buffer_.get();
  }

  /// @brief Resets the transients buffers held onto by the content context.
  void ResetTransientsBuffers();

  TextShadowCache& GetTextShadowCache() const { return *text_shadow_cache_; }

 protected:
  // Visible for testing.
  void SetTransientsIndexesBuffer(std::shared_ptr<HostBuffer> host_buffer) {
    indexes_host_buffer_ = std::move(host_buffer);
  }

  // Visible for testing.
  void SetTransientsDataBuffer(std::shared_ptr<HostBuffer> host_buffer) {
    data_host_buffer_ = std::move(host_buffer);
  }

 private:
  friend struct CoverageTiledRenderPassTestPeer;

  std::shared_ptr<Context> context_;
  std::shared_ptr<CoverageDisplayListPlan> coverage_classifier_storage_;
  std::shared_ptr<LazyGlyphAtlas> lazy_glyph_atlas_;

  /// Run backend specific additional setup and create common shader variants.
  ///
  /// This bootstrap is intended to improve the performance of several
  /// first frame benchmarks that are tracked in the flutter device lab.
  /// The workload includes initializing commonly used but not default
  /// shader variants, as well as forcing driver initialization.
  void InitializeCommonlyUsedShadersIfNeeded() const;

  /// Starts main's asynchronous legacy variant compile after its default.
  void PrewarmPipelineVariant(const PrewarmVariant& variant);
  bool PrewarmAvioCoveragePipelines() const;

  struct RuntimeEffectPipelineKey {
    std::string unique_entrypoint_name;
    ContentContextOptions options;

    struct Hash {
      std::size_t operator()(const RuntimeEffectPipelineKey& key) const {
        return fml::HashCombine(key.unique_entrypoint_name,
                                key.options.ToKey());
      }
    };

    struct Equal {
      inline bool operator()(const RuntimeEffectPipelineKey& lhs,
                             const RuntimeEffectPipelineKey& rhs) const {
        return lhs.unique_entrypoint_name == rhs.unique_entrypoint_name &&
               lhs.options.ToKey() == rhs.options.ToKey();
      }
    };
  };

  mutable std::unordered_map<RuntimeEffectPipelineKey,
                             std::shared_ptr<Pipeline<PipelineDescriptor>>,
                             RuntimeEffectPipelineKey::Hash,
                             RuntimeEffectPipelineKey::Equal>
      runtime_effect_pipelines_;

  struct Pipelines;
  std::unique_ptr<Pipelines> pipelines_;

  AvioPipelineInitialization pipeline_initialization_;
  mutable size_t avio_raster_frame_depth_ = 0u;
  std::shared_ptr<Tessellator> tessellator_;
  std::shared_ptr<RenderTargetAllocator> render_target_cache_;
  std::shared_ptr<HostBuffer> data_host_buffer_;
  std::shared_ptr<HostBuffer> indexes_host_buffer_;
  std::shared_ptr<Texture> empty_texture_;
  std::unique_ptr<TextShadowCache> text_shadow_cache_;
  std::unique_ptr<CoveragePathAtlas> coverage_path_atlas_;
  std::unique_ptr<AvioContinuousClipPool> continuous_clip_pool_;
  std::unique_ptr<AvioSample4ClipPool> sample4_clip_pool_;
  std::shared_ptr<CoverageRecorderStorage> coverage_recorder_storage_;
  std::shared_ptr<HostBuffer> continuous_data_host_buffer_;
  struct AvioPipelineVariants {
    std::shared_ptr<Pipeline<PipelineDescriptor>> source;
    bool ready = false;
    std::array<std::shared_ptr<Pipeline<PipelineDescriptor>>,
               static_cast<size_t>(AvioCoveragePipelineVariant::kCount)>
        variants;
  };
  static constexpr size_t kAvioPipelineVariantCapacity = 2048;
  mutable std::array<AvioPipelineVariants, kAvioPipelineVariantCapacity>
      avio_pipeline_variants_{};
  mutable std::mutex avio_pipeline_variants_mutex_;

  bool is_texture_caching_enabled_ = false;
  mutable std::unordered_map<const flutter::DlImage*, std::shared_ptr<Texture>>
      texture_cache_;

  ContentContext(const ContentContext&) = delete;

  ContentContext& operator=(const ContentContext&) = delete;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_CONTENT_CONTEXT_H_
