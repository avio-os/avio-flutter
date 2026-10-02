// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_TEXTURE_CONTENTS_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_TEXTURE_CONTENTS_H_

#include <algorithm>
#include <memory>

#include "flutter/display_list/dl_paint.h"
#include "impeller/core/sample4_source_proof.h"
#include "impeller/core/sampler_descriptor.h"
#include "impeller/entity/contents/contents.h"

namespace impeller {

class Texture;

/// Represents the contents of a texture to be rendered.
///
/// This class encapsulates a texture along with parameters defining how it
/// should be drawn, such as the source rectangle within the texture, the
/// destination rectangle on the render target, opacity, and sampler settings.
/// It's used by the rendering system to draw textured quads.
///
/// @see `TiledTextureContents` for a tiled version.
class TextureContents final : public Contents {
 public:
  TextureContents();

  ~TextureContents() override;

  /// A common case factory that marks the texture contents as having a
  /// destination rectangle.
  ///
  /// In this situation, a subpass can be avoided when image filters are
  /// applied.
  ///
  /// @param destination The destination rectangle in the Entity's local
  /// coordinate space.
  static std::shared_ptr<TextureContents> MakeRect(Rect destination);

  // Half-texel sampling bounds within the actual source view. A crop shorter
  // than one texel collapses that axis to its center, never reversed bounds
  // whose GLSL clamp result would be undefined.
  static Rect GetSourceSamplingBounds(const Rect& source) {
    const auto center = source.GetCenter();
    return Rect::MakeLTRB(std::min(source.GetLeft() + .5f, center.x),
                          std::min(source.GetTop() + .5f, center.y),
                          std::max(source.GetRight() - .5f, center.x),
                          std::max(source.GetBottom() - .5f, center.y));
  }

  // Ordinary native TextureFill evaluates its source at the pixel centre.
  // Only a real immutable capture can certify the same lane-uniform source
  // for the one-sample group. Own geometry must contain every accepted clip
  // lane. Unknown alpha keeps native4 partial pixels; the opaque group proof
  // additionally requires every extrapolated sample inside opaque texels.
  static std::optional<AvioSample4SourceProof> MakeCapturedSample4SourceProof(
      Rect destination,
      Rect source,
      const Matrix& transform,
      std::optional<Rect> captured_opaque_texels,
      Rect clip_bounds,
      IRect clip_raster_bounds,
      uint64_t segment_token,
      Scalar opacity,
      bool has_immutable_owner) {
    if (!has_immutable_owner || !segment_token || !std::isfinite(opacity) ||
        opacity < 0.f || opacity > 1.f || !destination.IsFinite() ||
        destination.IsEmpty() || !source.IsFinite() || source.IsEmpty() ||
        !clip_bounds.IsFinite() || clip_bounds.IsEmpty() ||
        clip_raster_bounds.IsEmpty() || !transform.IsFinite() ||
        !transform.IsAffine() || !transform.IsInvertible()) {
      return std::nullopt;
    }
    const auto inverse = transform.Invert();
    if (!inverse.IsFinite() ||
        !destination.Contains(clip_bounds.TransformBounds(inverse))) {
      return std::nullopt;
    }
    const auto to_source =
        Matrix::MakeTranslation(source.GetOrigin()) *
        Matrix::MakeScale(Vector2(source.GetSize() / destination.GetSize())) *
        Matrix::MakeTranslation(-destination.GetOrigin()) * inverse;
    if (!to_source.IsFinite()) {
      return std::nullopt;
    }
    const auto footprint =
        Rect::Make(clip_raster_bounds).TransformBounds(to_source).Expand(.5f);
    if (!footprint.IsFinite()) {
      return std::nullopt;
    }
    const bool opaque = opacity == 1.f && captured_opaque_texels &&
                        captured_opaque_texels->IsFinite() &&
                        captured_opaque_texels->Contains(footprint);
    return AvioSample4SourceProof{.segment_token = segment_token,
                                  .uniform_samples = true,
                                  .full_clip_geometry = true,
                                  .source_is_opaque = opaque,
                                  .captured_backdrop = true};
  }

  /// Sets a debug label for this contents object.
  ///
  /// This label is used for debugging purposes, for example, in graphics
  /// debuggers or logs.
  ///
  /// @param label The debug label string.
  void SetLabel(std::string_view label);

  /// Sets the destination rectangle within the current render target
  /// where the texture will be drawn.
  ///
  /// The texture, potentially clipped by the `source_rect_`, will be mapped to
  /// this rectangle. The coordinates are in the local coordinate space of the
  /// Entity.
  ///
  /// @param rect The destination rectangle in the Entity's local coordinate
  /// space.
  void SetDestinationRect(Rect rect);

  void SetTexture(std::shared_ptr<Texture> texture);

  std::shared_ptr<Texture> GetTexture() const;

  void SetResourceOwner(std::shared_ptr<void> owner);

  void SetCapturedOpaqueTexels(std::optional<Rect> texels) {
    captured_opaque_texels_ = texels;
  }
  void SetImmutableCapturedBackdrop(bool captured) {
    immutable_captured_backdrop_ = captured;
  }

  void SetSamplerDescriptor(const SamplerDescriptor& desc);

  const SamplerDescriptor& GetSamplerDescriptor() const;

  /// Sets the source rectangle within the texture to sample from.
  ///
  /// This rectangle defines the portion of the texture that will be mapped to
  /// the `destination_rect_`. The coordinates are in the coordinate space of
  /// the texture (texels), with the top-left corner being (0, 0).
  ///
  /// @param source_rect The rectangle defining the area of the texture to use.
  void SetSourceRect(const Rect& source_rect);

  const Rect& GetSourceRect() const;

  /// Sets whether strict source rect sampling should be used.
  ///
  /// When enabled, the texture coordinates are adjusted slightly (typically by
  /// half a texel) to ensure that linear filtering does not sample pixels
  /// outside the specified `source_rect_`. This is useful for preventing
  /// edge artifacts when rendering sub-sections of a texture atlas.
  ///
  /// @param strict True to enable strict source rect sampling, false otherwise.
  void SetStrictSourceRect(bool strict);

  bool GetStrictSourceRect() const;

  void SetOpacity(Scalar opacity);

  void SetCoverageMode(flutter::DlCoverageMode mode);

  void SetContinuousImageEdge(bool enabled);
  // Direct image scopes may omit the canonical four-lane geometry mask.
  // Unknown or overlapping image work keeps it enabled on native4 targets.
  void SetSample4ImageCoverage(bool enabled) {
    sample4_image_coverage_ = enabled;
  }
  // Only a complete classifier proof may resolve a canonical geometry mask
  // once into a1x source-over draw. Unknown/correlated scopes stay native4.
  void SetAnalyticSample4Image1x(bool enabled) {
    analytic_sample4_image_1x_ = enabled;
  }
  void SetDeferGeometryCoverage(bool defer) {
    defer_geometry_coverage_ = defer;
  }

  Scalar GetOpacity() const;

  void SetStencilEnabled(bool enabled);

  // |Contents|
  std::optional<Rect> GetCoverage(const Entity& entity) const override;

  // |Contents|
  std::optional<Snapshot> RenderToSnapshot(
      const ContentContext& renderer,
      const Entity& entity,
      const SnapshotOptions& options) const override;

  // |Contents|
  bool Render(const ContentContext& renderer,
              const Entity& entity,
              RenderPass& pass) const override;

  // |Contents|
  void SetInheritedOpacity(Scalar opacity) override;

  /// Sets whether applying the opacity should be deferred.
  ///
  /// When true, the opacity value (`GetOpacity()`) might not be applied
  /// directly during rendering operations like `RenderToSnapshot`. Instead, the
  /// opacity might be stored in the resulting `Snapshot` to be applied later
  /// when the snapshot is drawn. This is typically used as an optimization when
  /// the texture covers its destination rectangle completely and has near-full
  /// opacity, allowing the original texture to be used directly in the
  /// snapshot.
  ///
  /// @param defer_applying_opacity True to defer applying opacity, false to
  ///        apply it during rendering.
  void SetDeferApplyingOpacity(bool defer_applying_opacity);

  /// @see Snapshot::needs_rasterization_for_runtime_effects
  void SetNeedsRasterizationForRuntimeEffects(bool value);

 private:
  std::string label_;

  Rect destination_rect_;
  bool continuous_image_edge_ = false;
  bool sample4_image_coverage_ = true;
  bool analytic_sample4_image_1x_ = false;
  bool defer_geometry_coverage_ = false;
  bool stencil_enabled_ = true;

  std::shared_ptr<Texture> texture_;
  std::shared_ptr<void> resource_owner_;
  std::optional<Rect> captured_opaque_texels_;
  bool immutable_captured_backdrop_ = false;
  SamplerDescriptor sampler_descriptor_ = {};
  Rect source_rect_;
  bool strict_source_rect_enabled_ = false;
  Scalar opacity_ = 1.0f;
  Scalar inherited_opacity_ = 1.0f;
  bool defer_applying_opacity_ = false;
  bool snapshots_need_rasterization_for_runtime_effects_ = false;
  flutter::DlCoverageMode coverage_mode_ =
      flutter::DlCoverageMode::kPlatformDefault;

  TextureContents(const TextureContents&) = delete;

  TextureContents& operator=(const TextureContents&) = delete;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_TEXTURE_CONTENTS_H_
