// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "solid_color_contents.h"

#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/contents/sample4_clip.h"
#include "impeller/entity/entity.h"
#include "impeller/entity/geometry/geometry.h"
#include "impeller/renderer/render_pass.h"

namespace impeller {

SolidColorContents::SolidColorContents(const Geometry* geometry)
    : geometry_(geometry) {}

SolidColorContents::~SolidColorContents() = default;

void SolidColorContents::SetColor(Color color) {
  color_ = color;
}

Color SolidColorContents::GetColor() const {
  return color_.WithAlpha(color_.alpha * GetOpacityFactor());
}

const Geometry* SolidColorContents::GetGeometry() const {
  return geometry_;
}

bool SolidColorContents::IsSolidColor() const {
  return true;
}

bool SolidColorContents::IsOpaque(const Matrix& transform) const {
  return GetColor().IsOpaque() && !AppliesAlphaForStrokeCoverage(transform);
}

std::optional<Rect> SolidColorContents::GetCoverage(
    const Entity& entity) const {
  if (GetColor().IsTransparent()) {
    return std::nullopt;
  }

  const Geometry* geometry = GetGeometry();
  if (geometry == nullptr) {
    return std::nullopt;
  }
  return geometry->GetCoverage(entity.GetTransform());
};

bool SolidColorContents::Render(const ContentContext& renderer,
                                const Entity& entity,
                                RenderPass& pass) const {
  using VS = SolidFillPipeline::VertexShader;
  using FS = SolidFillPipeline::FragmentShader;
  auto& data_host_buffer = renderer.GetTransientsDataBuffer();

  VS::FrameInfo frame_info;
  FS::FragInfo frag_info;
  frag_info.color = GetColor().Premultiply() *
                    GetGeometry()->ComputeAlphaCoverage(entity.GetTransform());

  PipelineBuilderCallback pipeline_callback =
      [&renderer](ContentContextOptions options) {
        return renderer.GetSolidFillPipeline(options);
      };
  struct BindingState {
    FS::FragInfo& fragment;
    HostBuffer& buffer;
    const Entity& entity;
    const Geometry& geometry;
  } binding{frag_info, data_host_buffer, entity, *geometry_};
  return ColorSourceContents::DrawGeometry<VS>(
      renderer, entity, pass, pipeline_callback, frame_info,
      [&binding](RenderPass& pass) {
        const auto* clip = pass.GetAvioSample4Clip();
        const auto bounds =
            clip ? clip->GetSourceProofRasterBounds() : std::nullopt;
        if (bounds && binding.geometry.CoversArea(binding.entity.GetTransform(),
                                                  *bounds)) {
          const auto color = binding.fragment.color;
          pass.SetAvioSample4SourceProof(AvioSample4SourceProof{
              .segment_token = clip->segment_token,
              .uniform_samples = true,
              .full_clip_geometry = true,
              .source_is_opaque = color.w == 1.f,
              .source_is_encoded = color.w == 1.f &&
                                   (color.x == 0.f || color.x == 1.f) &&
                                   (color.y == 0.f || color.y == 1.f) &&
                                   (color.z == 0.f || color.z == 1.f)});
        }
        FS::BindFragInfo(pass, binding.buffer.EmplaceUniform(binding.fragment));
        pass.SetCommandLabel("Solid Fill");
        return true;
      });
}

std::optional<Color> SolidColorContents::AsBackgroundColor(
    const Entity& entity,
    ISize target_size) const {
  const Geometry* geometry = GetGeometry();
  if (geometry == nullptr) {
    return std::nullopt;
  }
  IRect target_rect = IRect::MakeSize(target_size);
  return geometry->CoversArea(entity.GetTransform(), target_rect)
             ? GetColor()
             : std::optional<Color>();
}

bool SolidColorContents::ApplyColorFilter(
    const ColorFilterProc& color_filter_proc) {
  color_ = color_filter_proc(color_);
  return true;
}

}  // namespace impeller
