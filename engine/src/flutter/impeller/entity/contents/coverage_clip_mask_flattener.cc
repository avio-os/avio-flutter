// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/coverage_clip_mask_flattener.h"

#include "impeller/entity/contents/clip_contents.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/contents/pipelines.h"
#include "impeller/entity/entity.h"
#include "impeller/renderer/blit_pass.h"
#include "impeller/renderer/command_buffer.h"
#include "impeller/renderer/context.h"
#include "impeller/renderer/render_pass.h"
#include "impeller/renderer/sampler_library.h"
#include "impeller/renderer/vertex_buffer_builder.h"

namespace impeller {
namespace {

bool ShaderRead(const std::shared_ptr<CommandBuffer>& commands,
                const std::shared_ptr<Texture>& texture) {
  auto barrier = commands->CreateBlitPass();
  return barrier && barrier->ConvertTextureToShaderRead(texture) &&
         barrier->EncodeCommands();
}

bool FillMembership(const ContentContext& renderer, RenderPass& pass) {
  using VS = SolidFillPipeline::VertexShader;
  using FS = SolidFillPipeline::FragmentShader;
  auto options = OptionsFromPass(pass);
  options.blend_mode = BlendMode::kSrc;
  options.stencil_mode = ContentContextOptions::StencilMode::kIgnore;
  options.primitive_type = PrimitiveType::kTriangleStrip;
  options.depth_compare = CompareFunction::kGreaterEqual;
  options.depth_write_enabled = false;
  auto pipeline = renderer.GetSolidFillPipeline(options);
  if (!pipeline) {
    return false;
  }
  auto& buffer = renderer.GetTransientsDataBuffer();
  VS::FrameInfo frame;
  // Every excluded lane has strictly positive clip depth. Accepted lanes keep
  // the full-clear depth zero, so this zero-depth fill captures their binary
  // membership without relying on the final draw's original clip ordinal.
  // Orthographic normally maps z=0 to .5. Explicitly zero it rather than
  // carrying the ordinary entity slice bias into this binary membership draw.
  frame.mvp =
      Matrix::MakeScale(Vector3{1, 1, 0}) * pass.GetOrthographicTransform();
  FS::FragInfo fragment;
  fragment.color = Vector4{1, 1, 1, 1};
  pass.SetCommandLabel("Flatten clip native sample membership");
  pass.SetPipeline(pipeline);
  return pass.SetVertexBuffer(CreateVertexBuffer(
             Rect::MakeSize(pass.GetRenderTargetSize()).GetPoints(), buffer)) &&
         VS::BindFrameInfo(pass, buffer.EmplaceUniform(frame)) &&
         FS::BindFragInfo(pass, buffer.EmplaceUniform(fragment)) &&
         pass.Draw().ok();
}

}  // namespace

AcquiredCoverageMask FlattenClipTile(
    const ContentContext& renderer,
    const std::shared_ptr<CommandBuffer>& commands,
    const std::shared_ptr<const AvioSample4ClipRecipe>& recipe,
    IRect raster) {
  auto failed =
      AcquiredCoverageMask{PreparedFillMaskStatus::kFailed, std::nullopt};
  auto region = renderer.GetAvioCoverageRegion();
  if (!commands || !recipe || !renderer.UsesAvioCoverage() || !region ||
      !region->CachesNativeMasks()) {
    return failed;
  }
  auto scratch = region->GetClipMaskScratchTarget();
  auto scratch_texture = scratch.GetColorAttachment(0).texture;
  if (!scratch_texture || !scratch.GetDepthAttachment() ||
      !scratch.GetStencilAttachment()) {
    return failed;
  }
  auto status =
      ValidateClipMaskTile(*recipe, raster, scratch.GetRenderTargetSize());
  if (status != PreparedFillMaskStatus::kPrepared) {
    return {status, std::nullopt};
  }
  // Claim the exclusive warm scratch content before any replay. This claim
  // is independent of persistent source masks, so a full cached-mask atlas
  // cannot deadlock a streamed deep clip. The consumer encodes before Drop.
  auto acquired = region->AcquireClipMaskScratch(raster.GetSize());
  if (acquired.status == AvioCoverageRegion::Status::kNeedsFlush ||
      acquired.status == AvioCoverageRegion::Status::kNeedsTiling) {
    return {PreparedFillMaskStatus::kDeferred, std::nullopt};
  }
  if (acquired.status != AvioCoverageRegion::Status::kSuccess ||
      !acquired.lease) {
    return failed;
  }
  // Persistent source masks remain in the atlas. Prepare their reads before
  // creating the scratch pass; the exclusive claim forbids a live prior
  // scratch source from being overwritten or fed back into its own attachment.
  if (!recipe->ForEachNode([&](const AvioSample4ClipNode& node) {
        return node.geometry || !node.mask ||
               ShaderRead(commands, node.mask->lease->GetRenderTarget()
                                        .GetColorAttachment(0)
                                        .texture);
      })) {
    return failed;
  }
  auto replay = commands->CreateRenderPass(scratch);
  if (!replay) {
    return failed;
  }
  const Vector2 offset{-Scalar(raster.GetLeft()), -Scalar(raster.GetTop())};
  auto remap = ClipMaskReplayTransform(recipe->logical_pass_size,
                                       scratch.GetRenderTargetSize(),
                                       raster.GetOrigin());
  if (!recipe->ForEachNode([&](const AvioSample4ClipNode& node) {
        if (node.geometry) {
          if (!node.geometry->vertex_buffer) {
            // An empty difference changes nothing; an empty intersect was
            // already returned as kEmpty before acquiring scratch.
            return node.operation == ClipOperation::kDifference;
          }
          auto geometry = *node.geometry;
          geometry.transform = remap * geometry.transform;
          ClipContents clip(node.GetBounds().Shift(offset), false);
          clip.SetGeometry(std::move(geometry));
          clip.SetClipOperation(node.operation);
          // Local depth is constant: each intersect/difference can only mark
          // further native lanes excluded. No stack depth enters 8-bit S8.
          return clip.Render(renderer, *replay, kClipMaskReplayLocalDepth);
        }
        if (node.quad) {
          auto quad = node.quad->Translated(offset);
          return CoverageMaskContents::RenderQuadClip(renderer, *replay, quad,
                                                      kClipMaskReplayLocalDepth,
                                                      node.operation);
        }
        auto mask = *node.mask;
        mask.target_rect = mask.target_rect.Shift(offset);
        mask.shape_bounds = mask.shape_bounds.Shift(offset);
        return CoverageMaskContents::RenderClip(
            renderer, *replay, std::span(&mask, 1), kClipMaskReplayLocalDepth,
            node.operation);
      }) ||
      !FillMembership(renderer, *replay) || !replay->EncodeCommands() ||
      !ShaderRead(commands, scratch_texture)) {
    return failed;
  }
  return {PreparedFillMaskStatus::kPrepared,
          CoverageMaskTile{std::move(acquired.lease), Rect::Make(raster),
                           Rect::Make(raster)}};
}
}  // namespace impeller
