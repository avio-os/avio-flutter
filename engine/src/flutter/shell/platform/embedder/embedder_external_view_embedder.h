// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_EXTERNAL_VIEW_EMBEDDER_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_EXTERNAL_VIEW_EMBEDDER_H_

#include <array>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

#include "flutter/flow/embedded_views.h"
#include "flutter/fml/hash_combine.h"
#include "flutter/fml/macros.h"
#include "flutter/shell/platform/embedder/embedder_external_view.h"
#include "flutter/shell/platform/embedder/embedder_render_target_cache.h"

namespace flutter {

/// Converts material nodes collected in Flutter scene coordinates into the
/// logical surface coordinates consumed by the embedder API. The external
/// view path prerolls with an identity root transform, then applies the
/// platform surface transform exactly once here, matching pixel rendering.
std::vector<FlutterAvioCompositorMaterial>
ConvertAvioCompositorMaterialsToEmbedderCoordinates(
    const std::vector<AvioCompositorMaterial>& materials,
    const DlMatrix& surface_transformation,
    double device_pixel_ratio);

/// Preview destinations and visible clips use the same root surface mapping.
std::vector<FlutterAvioWindowPreview>
ConvertAvioWindowPreviewsToEmbedderCoordinates(
    const std::vector<AvioWindowPreview>& previews,
    const DlMatrix& surface_transformation,
    double device_pixel_ratio);

/// Hit regions use the same root surface mapping, then become view-local
/// logical pixels. Writes the complete ordered set into the caller's inline
/// |out| without allocating and returns the number of regions written. The
/// caller passes only a valid set; an invalid one is refused upstream.
size_t ConvertAvioHitRegionsToEmbedderCoordinates(
    const AvioHitRegionSet& regions,
    const DlMatrix& surface_transformation,
    double device_pixel_ratio,
    std::array<FlutterAvioHitRegion, FLUTTER_AVIO_MAX_HIT_REGIONS>& out);

/// Everything a frame put on its target, in the physical-pixel space the root
/// view records in: the view's recorded draw-op bounds unioned with the rects
/// of the compositor materials the same frame published. Materials carry no
/// draw ops of their own, so the recording alone under-reports coverage by
/// exactly the glass surfaces.
DlRegion PaintCoverageForFrame(
    const EmbedderExternalView& root_view,
    const std::vector<AvioCompositorMaterial>& materials);

//------------------------------------------------------------------------------
/// @brief      The external view embedder used by the generic embedder API.
///             This class acts a proxy between the rasterizer and the embedder
///             when the rasterizer is rendering into multiple layers. It asks
///             the embedder for the render targets for the various layers the
///             rasterizer is rendering into, recycles the render targets as
///             necessary and converts rasterizer specific metadata into an
///             embedder friendly format so that it can present the layers
///             on-screen.
///
class EmbedderExternalViewEmbedder final : public ExternalViewEmbedder {
 public:
  using CreateRenderTargetCallback =
      std::function<std::unique_ptr<EmbedderRenderTarget>(
          GrDirectContext* context,
          const std::shared_ptr<impeller::AiksContext>& aiks_context,
          const FlutterBackingStoreConfig& config)>;
  struct RenderTargetAcquisition {
    ExternalViewEmbedder::RootRenderTargetAcquisition status;
    std::unique_ptr<EmbedderRenderTarget> target;
  };
  using AcquireRenderTargetCallback = std::function<RenderTargetAcquisition(
      GrDirectContext* context,
      const std::shared_ptr<impeller::AiksContext>& aiks_context,
      const FlutterBackingStoreConfig& config,
      FlutterFrameOpportunityId opportunity_id,
      FlutterEngineDisplayId display_id)>;
  using PresentCallback = std::function<bool(
      FlutterViewId view_id,
      const std::vector<const FlutterLayer*>& layers,
      const std::vector<FlutterAvioCompositorMaterial>& compositor_materials,
      bool compositor_materials_invalid,
      const AvioFrameFacts& frame_facts)>;
  using PresentRenderTargetCallback = std::function<bool(
      FlutterViewId view_id,
      FlutterFrameOpportunityId opportunity_id,
      FlutterEngineDisplayId display_id,
      FlutterPresentRenderTargetStatus status,
      const FlutterBackingStore* backing_store,
      const FlutterBackingStorePresentInfo* backing_store_present_info,
      const std::vector<FlutterAvioCompositorMaterial>& compositor_materials,
      bool compositor_materials_invalid,
      const std::vector<FlutterAvioWindowPreview>& window_previews,
      bool window_previews_invalid,
      const AvioFrameFacts& frame_facts,
      const FlutterAvioHitRegion* hit_regions,
      size_t hit_regions_count)>;
  using SurfaceTransformationCallback = std::function<DlMatrix(void)>;

  //----------------------------------------------------------------------------
  /// @brief      Creates an external view embedder used by the generic embedder
  ///             API.
  ///
  /// @param[in] avoid_backing_store_cache If set, create_render_target_callback
  ///                                      will beinvoked every frame for every
  ///                                      engine composited layer. The result
  ///                                      will not cached.
  ///
  /// @param[in]  create_render_target_callback
  ///                                     The render target callback used to
  ///                                     request the render target for a layer.
  /// @param[in]  present_callback        The callback used to forward a
  ///                                     collection of layers (backed by
  ///                                     fulfilled render targets) to the
  ///                                     embedder for presentation.
  ///
  EmbedderExternalViewEmbedder(
      FlutterCompositorMode compositor_mode,
      bool selected_target_damage,
      bool avoid_backing_store_cache,
      const CreateRenderTargetCallback& create_render_target_callback,
      const AcquireRenderTargetCallback& acquire_render_target_callback,
      const PresentCallback& present_callback,
      const PresentRenderTargetCallback& present_render_target_callback,
      FlutterAvioExtensionFeatures avio_frame_features = 0u);

  //----------------------------------------------------------------------------
  /// @brief      Collects the external view embedder.
  ///
  ~EmbedderExternalViewEmbedder() override;

  // |ExternalViewEmbedder|
  void CollectView(int64_t view_id) override;

  //----------------------------------------------------------------------------
  /// @brief      Sets the surface transformation callback used by the external
  ///             view embedder to ask the platform for the per frame root
  ///             surface transformation.
  ///
  /// @param[in]  surface_transformation_callback  The surface transformation
  ///                                              callback
  ///
  void SetSurfaceTransformationCallback(
      SurfaceTransformationCallback surface_transformation_callback);

 private:
  // |ExternalViewEmbedder|
  void CancelFrame() override;

  // |ExternalViewEmbedder|
  void BeginFrame(GrDirectContext* context,
                  const fml::RefPtr<fml::RasterThreadMerger>&
                      raster_thread_merger) override;

  // |ExternalViewEmbedder|
  void SetFrameOpportunity(
      std::optional<FrameOpportunityContext> frame_opportunity) override;

  // |ExternalViewEmbedder|
  void PrepareFlutterView(DlISize frame_size,
                          double device_pixel_ratio) override;

  // |ExternalViewEmbedder|
  std::optional<SurfaceFrame::FramebufferInfo> AcquireRootRenderTarget(
      int64_t flutter_view_id,
      GrDirectContext* context,
      const std::shared_ptr<impeller::AiksContext>& aiks_context) override;

  // |ExternalViewEmbedder|
  std::optional<ExternalViewEmbedder::RootRenderTargetAcquisition>
  GetRootRenderTargetAcquisition(int64_t flutter_view_id) const override;

  std::optional<RootRenderTargetResult> GetRootRenderTargetResult(
      int64_t flutter_view_id) const override;

  // |ExternalViewEmbedder|
  bool SupportsMetadataFrameDamageForCurrentFrame() const override;
  bool SupportsAvioEmptyFrames() const override;
  bool SupportsAvioFrameFacts(const AvioFrameFacts& facts) const override;
  bool SupportsAvioHitRegions() const override;
  bool SubmitAvioEmptyFrame(int64_t view_id,
                            const SurfaceFrame::SubmitInfo& info) override;
  void RejectAvioFrameFacts(int64_t view_id) override;

  // |ExternalViewEmbedder|
  void PrerollCompositeEmbeddedView(
      int64_t view_id,
      std::unique_ptr<EmbeddedViewParams> params) override;

  // |ExternalViewEmbedder|
  DlCanvas* CompositeEmbeddedView(int64_t view_id) override;

  // |ExternalViewEmbedder|
  void SubmitFlutterView(
      int64_t flutter_view_id,
      GrDirectContext* context,
      const std::shared_ptr<impeller::AiksContext>& aiks_context,
      std::unique_ptr<SurfaceFrame> frame) override;

  // |ExternalViewEmbedder|
  DlCanvas* GetRootCanvas() override;

 private:
  void SubmitGenericFlutterView(
      int64_t flutter_view_id,
      GrDirectContext* context,
      const std::shared_ptr<impeller::AiksContext>& aiks_context,
      std::unique_ptr<SurfaceFrame> frame);

  void SubmitRootRenderTarget(
      int64_t flutter_view_id,
      GrDirectContext* context,
      const std::shared_ptr<impeller::AiksContext>& aiks_context,
      std::unique_ptr<SurfaceFrame> frame);

  bool CompleteRootRenderTarget(
      int64_t flutter_view_id,
      FlutterPresentRenderTargetStatus status,
      const FlutterBackingStore* backing_store = nullptr,
      const FlutterBackingStorePresentInfo* backing_store_present_info =
          nullptr,
      const std::vector<FlutterAvioCompositorMaterial>* compositor_materials =
          nullptr,
      bool compositor_materials_invalid = false,
      const std::vector<FlutterAvioWindowPreview>* window_previews = nullptr,
      bool window_previews_invalid = false);

  const FlutterCompositorMode compositor_mode_;
  const bool selected_target_damage_;
  const bool avoid_backing_store_cache_;
  const FlutterAvioExtensionFeatures avio_frame_features_;
  AvioFrameFacts pending_frame_facts_;
  // The device-space claim of the revision being submitted, and the inline
  // scratch its logical conversion is borrowed from during the callback.
  AvioHitRegionSet pending_hit_regions_;
  std::array<FlutterAvioHitRegion, FLUTTER_AVIO_MAX_HIT_REGIONS>
      hit_region_scratch_{};
  const CreateRenderTargetCallback create_render_target_callback_;
  const AcquireRenderTargetCallback acquire_render_target_callback_;
  const PresentCallback present_callback_;
  const PresentRenderTargetCallback present_render_target_callback_;
  SurfaceTransformationCallback surface_transformation_callback_;
  DlISize pending_frame_size_;
  double pending_device_pixel_ratio_ = 1.0;
  DlMatrix pending_surface_transformation_;
  EmbedderExternalView::PendingViews pending_views_;
  std::vector<EmbedderExternalView::ViewIdentifier> composition_order_;
  std::optional<FrameOpportunityContext> pending_frame_opportunity_;
  std::optional<int64_t> pending_root_view_id_;
  std::optional<EmbedderExternalView::RenderTargetDescriptor>
      pending_root_descriptor_;
  std::unique_ptr<EmbedderRenderTarget> pending_root_render_target_;
  std::set<std::unique_ptr<EmbedderRenderTarget>>
      pending_root_deferred_cleanup_render_targets_;
  // Exact acquisition result for each root target selected in this frame.
  // Cleared at every frame boundary.
  std::unordered_map<int64_t, RootRenderTargetResult> root_target_results_;
  std::unordered_map<int64_t, ExternalViewEmbedder::RootRenderTargetAcquisition>
      root_target_acquisitions_;
  // The render target caches for views. Each key is a view ID.
  std::unordered_map<int64_t, EmbedderRenderTargetCache> render_target_caches_;
  // Full logical paint coverage for each root view. Partial EVE recordings
  // replace only their buffer-damage region in this retained coverage.
  std::unordered_map<int64_t, DlRegion> root_paint_regions_;

  void Reset();

  void ResetPendingRootRenderTarget();

  DlMatrix GetSurfaceTransformation() const;

  FML_DISALLOW_COPY_AND_ASSIGN(EmbedderExternalViewEmbedder);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_EXTERNAL_VIEW_EMBEDDER_H_
