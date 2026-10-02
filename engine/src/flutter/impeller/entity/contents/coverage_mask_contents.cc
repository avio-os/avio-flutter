// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/coverage_mask_contents.h"

#include <cmath>

#include "impeller/entity/contents/clip_operation_scope.h"
#include "impeller/entity/contents/color_source_contents.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/contents/contents.h"
#include "impeller/entity/contents/coverage_path_atlas.h"
#include "impeller/entity/contents/pipelines.h"
#include "impeller/entity/entity.h"
#include "impeller/entity/geometry/coverage_geometry.h"
#include "impeller/entity/geometry/fill_path_geometry.h"
#include "impeller/renderer/context.h"
#include "impeller/renderer/render_pass.h"
#include "impeller/renderer/sampler_library.h"
#include "impeller/renderer/vertex_buffer_builder.h"

namespace impeller {
namespace {

uint64_t CoveragePixelArea(Rect bounds, const RenderPass& pass) {
  auto clipped =
      bounds.Intersection(Rect::MakeSize(pass.GetRenderTargetSize()));
  if (!clipped || !clipped->IsFinite() || clipped->IsEmpty()) {
    return 0;
  }
  auto integral = IRect::RoundOut(*clipped);
  return static_cast<uint64_t>(integral.GetWidth()) *
         static_cast<uint64_t>(integral.GetHeight());
}

bool WriteClipDepth(const ContentContext& renderer,
                    RenderPass& pass,
                    Scalar shader_depth,
                    ClipOperation operation) {
  using VS = ClipPipeline::VertexShader;
  auto options = OptionsFromPass(pass);
  options.blend_mode = BlendMode::kDst;
  options.primitive_type = PrimitiveType::kTriangleStrip;
  options.depth_write_enabled = true;
  options.stencil_mode =
      operation == ClipOperation::kIntersect
          ? ContentContextOptions::StencilMode::kCoverCompareInverted
          : ContentContextOptions::StencilMode::kCoverCompare;
  auto pipeline = renderer.GetClipPipeline(options);
  if (!pipeline || !pass.HasDepthAttachment()) {
    return false;
  }
  VS::FrameInfo frame;
  frame.mvp = pass.GetOrthographicTransform();
  frame.depth = shader_depth;
  pass.SetCommandLabel("Coverage clip depth");
  pass.SetPipeline(pipeline);
  if (!pass.SetVertexBuffer(CreateVertexBuffer(
          Rect::MakeSize(pass.GetRenderTargetSize()).GetPoints(),
          renderer.GetTransientsDataBuffer()))) {
    return false;
  }
  return VS::BindFrameInfo(
             pass, renderer.GetTransientsDataBuffer().EmplaceUniform(frame)) &&
         pass.Draw().ok();
}

}  // namespace

AcquiredCoverageMask CoverageMaskContents::TryAcquireClipPathMask(
    const ContentContext& renderer,
    const Matrix& physical_transform,
    ISize logical_pass_size,
    const Geometry& geometry) {
  auto path = geometry.GetFillPath();
  auto atlas = renderer.GetCoveragePathAtlas();
  if (!renderer.UsesAvioCoverage() || !atlas || !path) {
    return {PreparedFillMaskStatus::kNotApplicable, std::nullopt};
  }
  auto coverage = geometry.GetCoverage(physical_transform);
  if (!coverage || coverage->IsEmpty()) {
    return {PreparedFillMaskStatus::kEmpty, std::nullopt};
  }
  auto region = renderer.GetAvioCoverageRegion();
  if (!region) {
    return {PreparedFillMaskStatus::kFailed, std::nullopt};
  }
  auto plan = CoverageAtlas::PlanTiles(
      coverage->Expand(1), IRect::MakeSize(logical_pass_size),
      region->GetCoverageTileSize(AvioCoverageRegion::Kind::kMask));
  if (!plan) {
    return {PreparedFillMaskStatus::kFailed, std::nullopt};
  }
  auto tile = plan->Next();
  if (!tile) {
    return {PreparedFillMaskStatus::kEmpty, std::nullopt};
  }
  // A whole logical pass may defer source reads until colour-tile replay. A
  // large path cannot hold more masks than the fixed atlas permits. Its
  // original winding geometry is already replayed through bounded islands.
  if (plan->Next()) {
    return {PreparedFillMaskStatus::kNotApplicable, std::nullopt};
  }
  auto acquired = atlas->AcquirePathMask(
      *path, physical_transform, *tile,
      [&](const AvioCoverageRegion::Lease& lease, const Matrix& transform) {
        auto command_buffer = renderer.GetContext()->CreateCommandBuffer();
        if (!command_buffer) {
          return false;
        }
        auto target = lease.GetRenderTarget();
        FillPathGeometry mask_geometry(*path);
        auto rendered = renderer.MakeSubpass(
            "Cached binary fill path", target, command_buffer,
            [&](const ContentContext& renderer, RenderPass& mask_pass) {
              using VS = SolidFillPipeline::VertexShader;
              using FS = SolidFillPipeline::FragmentShader;
              Entity mask_entity;
              mask_entity.SetTransform(transform);
              mask_entity.SetBlendMode(BlendMode::kSrcOver);
              VS::FrameInfo frame;
              FS::FragInfo fragment;
              fragment.color = Vector4{1, 1, 1, 1};
              return ColorSourceContents::DrawGeometry<VS>(
                  nullptr, &mask_geometry, renderer, mask_entity, mask_pass,
                  [&](ContentContextOptions options) {
                    return renderer.GetSolidFillPipeline(options);
                  },
                  frame,
                  [&](RenderPass& current) {
                    return FS::BindFragInfo(
                        current,
                        renderer.GetTransientsDataBuffer().EmplaceUniform(
                            fragment));
                  },
                  false,
                  [](const ContentContext& renderer, const Entity& entity,
                     RenderPass& pass, const Geometry* geometry) {
                    return geometry->GetPositionBuffer(renderer, entity, pass);
                  },
                  false);
            });
        return rendered.ok() && renderer.GetContext()->EnqueueCommandBuffer(
                                    std::move(command_buffer));
      });
  if (!acquired) {
    return {PreparedFillMaskStatus::kFailed, std::nullopt};
  }
  if (acquired->status == AvioCoverageRegion::Status::kNeedsFlush ||
      acquired->status == AvioCoverageRegion::Status::kNeedsTiling) {
    // Pressure selects the existing bounded native geometry path before any
    // destination stencil mutation. It never grows or resets a live atlas.
    return {PreparedFillMaskStatus::kNotApplicable, std::nullopt};
  }
  if (acquired->status != AvioCoverageRegion::Status::kSuccess ||
      !acquired->lease) {
    return {PreparedFillMaskStatus::kFailed, std::nullopt};
  }
  return {PreparedFillMaskStatus::kPrepared,
          CoverageMaskTile{acquired->lease, Rect::Make(tile->raster_rect),
                           *coverage}};
}

PreparedFillMaskStatus CoverageMaskContents::TryPrepareFillPath(
    const ContentContext& renderer,
    const Entity& entity,
    RenderPass& pass,
    const Geometry& geometry) {
  if (pass.GetSampleCount() != SampleCount::kCount4) {
    return PreparedFillMaskStatus::kNotApplicable;
  }
  auto acquired = TryAcquireClipPathMask(renderer, entity.GetTransform(),
                                         pass.GetRenderTargetSize(), geometry);
  if (acquired.status != PreparedFillMaskStatus::kPrepared) {
    return acquired.status;
  }
  if (!PrepareStencil(renderer, pass, std::span(&acquired.mask.value(), 1),
                      entity.GetShaderClipDepth())) {
    return PreparedFillMaskStatus::kFailed;
  }
  renderer.GetContext()->RecordAvioCoverageDraw(
      AvioCoverageReason::kCachedFillMask,
      CoveragePixelArea(acquired.mask->shape_bounds, pass));
  return PreparedFillMaskStatus::kPrepared;
}

bool CoverageMaskContents::PrepareStencil(
    const ContentContext& renderer,
    RenderPass& pass,
    std::span<const CoverageMaskTile> masks,
    Scalar shader_depth) {
#ifndef IMPELLER_ENABLE_VULKAN
  return false;
#else
  if (!renderer.UsesAvioCoverage() ||
      renderer.GetContext()->GetBackendType() !=
          Context::BackendType::kVulkan ||
      pass.GetSampleCount() != SampleCount::kCount4 ||
      !pass.HasStencilAttachment() || !std::isfinite(shader_depth)) {
    return false;
  }
  using VS = CoverageMaskPipeline::VertexShader;
  using FS = CoverageMaskPipeline::FragmentShader;
  auto& host_buffer = renderer.GetTransientsDataBuffer();
  auto options = OptionsFromPass(pass);
  options.blend_mode = BlendMode::kDst;
  options.primitive_type = PrimitiveType::kTriangleStrip;
  options.depth_write_enabled = false;
  options.stencil_mode =
      ContentContextOptions::StencilMode::kStencilIncrementAll;
  auto pipeline = renderer.GetCoverageMaskPipeline(options);
  if (!pipeline) {
    return false;
  }
  pass.SetStencilReference(0);
  for (const auto& mask : masks) {
    if (!mask.lease || !mask.lease->IsValid() ||
        mask.lease->GetKind() != AvioCoverageRegion::Kind::kMask ||
        !mask.target_rect.IsFinite() || mask.target_rect.IsEmpty() ||
        std::floor(mask.target_rect.GetLeft()) != mask.target_rect.GetLeft() ||
        std::floor(mask.target_rect.GetTop()) != mask.target_rect.GetTop() ||
        mask.target_rect.GetSize() != Size(mask.lease->GetRequestedSize())) {
      return false;
    }
    auto texture = mask.lease->GetRenderTarget().GetColorAttachment(0).texture;
    if (!texture ||
        texture->GetTextureDescriptor().format != PixelFormat::kR8UNormInt ||
        texture->GetTextureDescriptor().sample_count != SampleCount::kCount4 ||
        !(texture->GetTextureDescriptor().usage & TextureUsage::kShaderRead)) {
      return false;
    }
    auto source = Rect::Make(mask.lease->GetContentRect());
    auto target_points = mask.target_rect.GetPoints();
    auto source_points = source.GetPoints();
    std::array<VS::PerVertexData, 4> vertices;
    for (size_t index = 0; index < vertices.size(); index++) {
      vertices[index] = {target_points[index], source_points[index]};
    }
    pass.RetainResource(mask.lease);
    pass.SetCommandLabel("Native four-sample cached coverage");
    pass.SetPipeline(pipeline);
    if (!pass.SetVertexBuffer(CreateVertexBuffer(vertices, host_buffer))) {
      return false;
    }
    VS::FrameInfo frame;
    frame.mvp = pass.GetOrthographicTransform();
    frame.depth = shader_depth;
    if (!VS::BindFrameInfo(pass, host_buffer.EmplaceUniform(frame)) ||
        !FS::BindMaskSampler(
            pass, texture,
            renderer.GetContext()->GetSamplerLibrary()->GetSampler({})) ||
        !pass.Draw().ok()) {
      return false;
    }
  }
  return true;
#endif
}

bool CoverageMaskContents::RenderClip(const ContentContext& renderer,
                                      RenderPass& pass,
                                      std::span<const CoverageMaskTile> masks,
                                      uint32_t clip_depth,
                                      ClipOperation operation) {
  AvioClipOperationScope clip_operation(pass);
  auto shader_depth =
      std::nextafterf(Entity::GetShaderClipDepth(clip_depth + 1), 0.0f);
  if (!PrepareStencil(renderer, pass, masks, shader_depth)) {
    return false;
  }
  if (!WriteClipDepth(renderer, pass, shader_depth, operation)) {
    return false;
  }
  std::optional<Rect> bounds;
  for (const auto& mask : masks) {
    bounds = bounds ? bounds->Union(mask.shape_bounds) : mask.shape_bounds;
  }
  renderer.GetContext()->RecordAvioCoverageDraw(
      AvioCoverageReason::kCachedClipMask,
      bounds ? CoveragePixelArea(*bounds, pass) : 0);
  return true;
}

bool CoverageMaskContents::RenderQuadClip(const ContentContext& renderer,
                                          RenderPass& pass,
                                          const CoverageConvexQuad4& quad,
                                          uint32_t clip_depth,
                                          ClipOperation operation) {
  AvioClipOperationScope clip_operation(pass);
#ifndef IMPELLER_ENABLE_VULKAN
  return false;
#else
  if (!renderer.UsesAvioCoverage() ||
      renderer.GetContext()->GetBackendType() !=
          Context::BackendType::kVulkan ||
      pass.GetSampleCount() != SampleCount::kCount4 ||
      !pass.HasStencilAttachment() || !pass.HasDepthAttachment()) {
    return false;
  }
  using VS = CoverageQuadPipeline::VertexShader;
  using FS = CoverageQuadPipeline::FragmentShader;
  auto& host_buffer = renderer.GetTransientsDataBuffer();
  auto options = OptionsFromPass(pass);
  options.blend_mode = BlendMode::kDst;
  options.primitive_type = PrimitiveType::kTriangleStrip;
  options.depth_write_enabled = false;
  options.stencil_mode =
      ContentContextOptions::StencilMode::kStencilIncrementAll;
  auto pipeline = renderer.GetCoverageQuadPipeline(options);
  if (!pipeline) {
    return false;
  }
  auto shader_depth =
      std::nextafterf(Entity::GetShaderClipDepth(clip_depth + 1), 0.0f);
  VS::FrameInfo frame;
  frame.mvp = pass.GetOrthographicTransform();
  frame.depth = shader_depth;
  FS::QuadInfo info;
  info.lines = quad.GetLineParameters();
  // Replay shifts the physical viewport. These vertices and line equations
  // stay on the original logical pass grid, preserving fractional sample phase.
  pass.SetCommandLabel("Analytic four-sample quad clip");
  pass.SetStencilReference(0);
  pass.SetPipeline(pipeline);
  if (!pass.SetVertexBuffer(CreateVertexBuffer(
          Rect::MakeSize(pass.GetRenderTargetSize()).GetPoints(),
          host_buffer)) ||
      !VS::BindFrameInfo(pass, host_buffer.EmplaceUniform(frame)) ||
      !FS::BindQuadInfo(pass, host_buffer.EmplaceUniform(info)) ||
      !pass.Draw().ok() ||
      !WriteClipDepth(renderer, pass, shader_depth, operation)) {
    return false;
  }
  renderer.GetContext()->RecordAvioCoverageDraw(
      AvioCoverageReason::kAnalyticQuadClip,
      CoveragePixelArea(quad.GetBounds(), pass));
  return true;
#endif
}

}  // namespace impeller
