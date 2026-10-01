// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/uber_sdf_contents.h"

#include <vector>

#include "flutter/fml/logging.h"
#include "impeller/entity/contents/color_source_contents.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/contents/gradient_generator.h"
#include "impeller/entity/contents/pipelines.h"
#include "impeller/entity/contents/uber_sdf_parameters.h"
#include "impeller/geometry/stroke_parameters.h"

namespace impeller {

namespace {

using PipelineBuilderCallback =
    std::function<PipelineRef(ContentContextOptions)>;

Scalar ToShaderType(UberSDFParameters::Type type) {
  switch (type) {
    case UberSDFParameters::Type::kCircle:
      return 0.0f;
    case UberSDFParameters::Type::kRect:
      return 1.0f;
    case UberSDFParameters::Type::kOval:
      return 2.0f;
    case UberSDFParameters::Type::kRoundedRect:
      return 3.0f;
    case UberSDFParameters::Type::kRoundedSuperellipseSymmetric:
      return 4.0f;
  }
}

Scalar ToShaderStrokeJoin(Join join) {
  switch (join) {
    case Join::kMiter:
      return 0.0f;
    case Join::kBevel:
      return 1.0f;
    case Join::kRound:
      return 2.0f;
  }
}

Scalar ToShaderColorSourceType(const UberSDFParameters& params) {
  if (!params.gradient.has_value()) {
    return 0.0f;
  }
  switch (params.gradient->type) {
    case UberSDFParameters::GradientParameters::Type::kLinear:
      return 1.0f;
    case UberSDFParameters::GradientParameters::Type::kRadial:
      return 2.0f;
  }
}

/// @brief  Populates the `frag_info` fields shared by both UberSDF variants.
template <typename FragInfo>
void SetupCommonFragInfo(const UberSDFParameters& params,
                         Scalar opacity,
                         bool defer_coverage_transform,
                         flutter::DlCoverageMode coverage_mode,
                         FragInfo& frag_info) {
  frag_info.type = ToShaderType(params.type);
  frag_info.color = params.color.WithAlpha(params.color.alpha * opacity);
  frag_info.center = params.center;
  frag_info.size = params.size;
  frag_info.stroked = params.stroke ? 1.0f : 0.0f;
  frag_info.stroke_width = params.stroke ? params.stroke->width : 0.0f;
  frag_info.stroke_join =
      params.stroke ? ToShaderStrokeJoin(params.stroke->join) : 0.0f;
  frag_info.aa_pixels = UberSDFParameters::kAntialiasPixels;
  frag_info.superellipse_degree = params.superellipse_degree;
  frag_info.superellipse_semi_axis = params.superellipse_semi_axis;
  frag_info.angle_span = params.angle_span;
  frag_info.octant_offset_c = params.octant_offset_c;
  frag_info.circle_center_top = params.circle_center_top;
  frag_info.circle_center_right = params.circle_center_right;
  frag_info.superellipse_scale = params.superellipse_scale;
  frag_info.radii = params.radii;
  frag_info.defer_coverage_transform = defer_coverage_transform ? 1.0f : 0.0f;
  frag_info.external_linear_backdrop =
      coverage_mode == flutter::DlCoverageMode::kExternalLinearBackdrop ? 1.0f
                                                                        : 0.0f;

  // Gradient defaults; overwritten below when there is a gradient.
  frag_info.color_source_type = ToShaderColorSourceType(params);
  frag_info.gradient_start = Point();
  frag_info.gradient_end = Point();
  frag_info.half_texel = Point();
  frag_info.tile_mode = 0.0f;
  frag_info.inverse_gradient_length = 0.0f;
  frag_info.colors_length = 0.0f;

  if (!params.gradient.has_value()) {
    return;
  }
  const UberSDFParameters::GradientParameters& gradient =
      params.gradient.value();
  frag_info.gradient_start = gradient.start;
  frag_info.gradient_end = gradient.end;
  frag_info.tile_mode = static_cast<Scalar>(gradient.tile_mode);
  if (gradient.type == UberSDFParameters::GradientParameters::Type::kLinear) {
    const Point delta = gradient.end - gradient.start;
    const Scalar length_sq = delta.x * delta.x + delta.y * delta.y;
    frag_info.inverse_gradient_length =
        length_sq == 0.0f ? 0.0f : 1.0f / length_sq;
  } else {
    frag_info.inverse_gradient_length =
        gradient.end.x > 0.0f ? 1.0f / gradient.end.x : 0.0f;
  }
}

}  // namespace

std::unique_ptr<UberSDFContents> UberSDFContents::Make(
    const UberSDFParameters& params,
    std::unique_ptr<Geometry> geometry) {
  return std::unique_ptr<UberSDFContents>(
      new UberSDFContents(params, std::move(geometry)));
}

UberSDFContents::UberSDFContents(const UberSDFParameters& params,
                                 std::unique_ptr<Geometry> geometry)
    : params_(params), geometry_(std::move(geometry)) {}

UberSDFContents::~UberSDFContents() = default;

void UberSDFContents::SetCoverageMode(flutter::DlCoverageMode mode) {
  coverage_mode_ = mode;
}

bool UberSDFContents::Render(const ContentContext& renderer,
                             const Entity& entity,
                             RenderPass& pass) const {
  if (renderer.GetDeviceCapabilities().SupportsSSBO()) {
    return RenderSSBO(renderer, entity, pass);
  }
  return RenderTexture(renderer, entity, pass);
}

bool UberSDFContents::RenderTexture(const ContentContext& renderer,
                                    const Entity& entity,
                                    RenderPass& pass) const {
  using VS = UberSDFPipeline::VertexShader;
  using FS = UberSDFPipeline::FragmentShader;

  auto& data_host_buffer = renderer.GetTransientsDataBuffer();

  VS::FrameInfo frame_info;
  FS::FragInfo frag_info;
  SetupCommonFragInfo(params_, GetOpacityFactor(), defer_coverage_transform_,
                      coverage_mode_, frag_info);

  std::shared_ptr<Texture> texture;
  raw_ptr<const Sampler> sampler;
  if (params_.gradient.has_value()) {
    FML_DCHECK(params_.gradient->texture);
    texture = params_.gradient->texture;
    const ISize texture_size = texture->GetSize();
    FML_DCHECK(!texture_size.IsEmpty());
    frag_info.half_texel =
        Point(0.5f / texture_size.width, 0.5f / texture_size.height);
    SamplerDescriptor sampler_desc;
    sampler_desc.min_filter = MinMagFilter::kLinear;
    sampler_desc.mag_filter = MinMagFilter::kLinear;
    sampler =
        renderer.GetContext()->GetSamplerLibrary()->GetSampler(sampler_desc);
  } else {
    // A solid color ignores the sampler, but it must be bound.
    texture = renderer.GetEmptyTexture();
    sampler = renderer.GetContext()->GetSamplerLibrary()->GetSampler({});
  }

  auto geometry_result =
      GetGeometry()->GetPositionBuffer(renderer, entity, pass);

  PipelineBuilderCallback pipeline_callback =
      [&renderer](ContentContextOptions options) {
        return renderer.GetUberSDFPipeline(options);
      };

  return ColorSourceContents::DrawGeometry<VS>(
      this, GetGeometry(), renderer, entity, pass, pipeline_callback,
      frame_info,
      /*bind_fragment_callback=*/
      [&frag_info, &data_host_buffer, texture = std::move(texture),
       sampler](RenderPass& pass) {
        FS::BindColorSourceSampler(pass, texture, sampler);
        FS::BindFragInfo(pass, data_host_buffer.EmplaceUniform(frag_info));
        pass.SetCommandLabel("UberSDF");
        return true;
      },
      /*force_stencil=*/false,
      /*create_geom_callback=*/
      [geometry_result = std::move(geometry_result)](
          const ContentContext& renderer, const Entity& entity,
          RenderPass& pass,
          const Geometry* geometry) { return geometry_result; });
}

bool UberSDFContents::RenderSSBO(const ContentContext& renderer,
                                 const Entity& entity,
                                 RenderPass& pass) const {
  using VS = UberSDFSSBOPipeline::VertexShader;
  using FS = UberSDFSSBOPipeline::FragmentShader;

  auto& data_host_buffer = renderer.GetTransientsDataBuffer();

  VS::FrameInfo frame_info;
  FS::FragInfo frag_info;
  SetupCommonFragInfo(params_, GetOpacityFactor(), defer_coverage_transform_,
                      coverage_mode_, frag_info);

  std::vector<StopData> color_stops;
  if (params_.gradient.has_value()) {
    color_stops =
        CreateGradientColors(params_.gradient->colors, params_.gradient->stops);
  }
  if (color_stops.empty()) {
    // The bound storage buffer must not be empty. A solid color ignores it.
    color_stops.resize(1);
  }
  frag_info.colors_length = params_.gradient.has_value()
                                ? static_cast<Scalar>(color_stops.size())
                                : 0.0f;

  BufferView color_buffer = data_host_buffer.Emplace(
      color_stops.data(), color_stops.size() * sizeof(StopData),
      renderer.GetDeviceCapabilities().GetMinimumStorageBufferAlignment());

  auto geometry_result =
      GetGeometry()->GetPositionBuffer(renderer, entity, pass);

  PipelineBuilderCallback pipeline_callback =
      [&renderer](ContentContextOptions options) {
        return renderer.GetUberSDFSSBOPipeline(options);
      };

  return ColorSourceContents::DrawGeometry<VS>(
      this, GetGeometry(), renderer, entity, pass, pipeline_callback,
      frame_info,
      /*bind_fragment_callback=*/
      [&frag_info, &data_host_buffer, &color_buffer](RenderPass& pass) {
        FS::BindFragInfo(pass, data_host_buffer.EmplaceUniform(frag_info));
        FS::BindColorData(pass, color_buffer);
        pass.SetCommandLabel("UberSDFSSBO");
        return true;
      },
      /*force_stencil=*/false,
      /*create_geom_callback=*/
      [geometry_result = std::move(geometry_result)](
          const ContentContext& renderer, const Entity& entity,
          RenderPass& pass,
          const Geometry* geometry) { return geometry_result; });
}

std::optional<Rect> UberSDFContents::GetCoverage(const Entity& entity) const {
  return GetGeometry()->GetCoverage(entity.GetTransform());
}

const Geometry* UberSDFContents::GetGeometry() const {
  return geometry_.get();
}

Color UberSDFContents::GetColor() const {
  return params_.color;
}

bool UberSDFContents::ApplyColorFilter(
    const ColorFilterProc& color_filter_proc) {
  if (params_.gradient.has_value()) {
    // A gradient's colors are not a single CPU color; the filter must wrap
    // these contents on the GPU instead.
    return false;
  }
  params_.color = color_filter_proc(params_.color);
  return true;
}

std::optional<Color> UberSDFContents::AsBackgroundColor(
    const Entity& entity,
    ISize target_size) const {
  if (params_.type != UberSDFParameters::Type::kRect ||
      params_.gradient.has_value()) {
    return std::nullopt;
  }
  const Geometry* geometry = GetGeometry();
  if (geometry == nullptr) {
    return std::nullopt;
  }
  IRect target_rect = IRect::MakeSize(target_size);
  return geometry->CoversArea(entity.GetTransform(), target_rect)
             ? GetColor()
             : std::optional<Color>();
}

}  // namespace impeller
