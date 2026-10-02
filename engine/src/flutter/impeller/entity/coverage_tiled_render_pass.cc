// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/coverage_tiled_render_pass.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

#include "fml/logging.h"
#include "impeller/entity/avio_coverage_region.h"
#include "impeller/entity/contents/clip_contents.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/contents/coverage_atlas.h"
#include "impeller/entity/contents/coverage_clip_mask_flattener.h"
#include "impeller/entity/contents/sample4_clip_uniform.h"
#include "impeller/entity/contents/texture_contents.h"
#include "impeller/entity/entity.h"
#include "impeller/renderer/blit_pass.h"
#include "impeller/renderer/context.h"
#include "impeller/renderer/continuous_coverage_pipeline.h"
#include "impeller/renderer/render_resource_scope.h"
#include "impeller/renderer/shader_function.h"

namespace impeller {
namespace {

bool IsSrcOverSource(const ColorAttachmentDescriptor& colour, bool opaque) {
  if (!colour.blending_enabled)
    return opaque;
  return colour.color_blend_op == BlendOperation::kAdd &&
         colour.alpha_blend_op == BlendOperation::kAdd &&
         colour.src_color_blend_factor == BlendFactor::kOne &&
         colour.src_alpha_blend_factor == BlendFactor::kOne &&
         ((colour.dst_color_blend_factor == BlendFactor::kOneMinusSourceAlpha &&
           colour.dst_alpha_blend_factor ==
               BlendFactor::kOneMinusSourceAlpha) ||
          (opaque && colour.dst_color_blend_factor == BlendFactor::kZero &&
           colour.dst_alpha_blend_factor == BlendFactor::kZero));
}

SamplerDescriptor PixelPrefixSampler() {
  SamplerDescriptor sampler;
  sampler.min_filter = MinMagFilter::kNearest;
  sampler.mag_filter = MinMagFilter::kNearest;
  sampler.mip_filter = MipFilter::kBase;
  return sampler;
}

// A stack decorator changes only this known texture-copy shader family. The
// underlying native pass owns all resources and encodes the actual draw.
class Sample4LayerPass final : public RenderPass {
 public:
  Sample4LayerPass(const ContentContext& renderer,
                   RenderPass& native,
                   const AvioSample4ClipDescriptor& clip)
      : RenderPass(native.GetContext(), native.GetRenderTarget()),
        renderer_(renderer),
        native_(native),
        clip_(clip) {}
  bool IsValid() const override { return valid_ && native_.IsValid(); }
  void SetPipeline(PipelineRef source) override {
    auto variant = renderer_.GetAvioPipelineVariant(
        source.Lock(), AvioCoveragePipelineVariant::kLayerSample4);
    valid_ = valid_ && variant != nullptr;
    if (variant)
      native_.SetPipeline(variant);
  }
  void SetCommandLabel(std::string_view label) override {
    native_.SetCommandLabel(label);
  }
  void SetStencilReference(uint32_t value) override {
    native_.SetStencilReference(value);
  }
  void SetBaseVertex(uint64_t value) override { native_.SetBaseVertex(value); }
  void SetViewport(Viewport value) override { native_.SetViewport(value); }
  void SetScissor(IRect32 value) override { native_.SetScissor(value); }
  void SetElementCount(size_t value) override {
    native_.SetElementCount(value);
  }
  void SetInstanceCount(size_t value) override {
    native_.SetInstanceCount(value);
  }
  bool SetVertexBuffer(BufferView buffers[], size_t count) override {
    return native_.SetVertexBuffer(buffers, count);
  }
  bool SetIndexBuffer(BufferView buffer, IndexType type) override {
    return native_.SetIndexBuffer(std::move(buffer), type);
  }
  bool BindResource(ShaderStage stage,
                    DescriptorType type,
                    const ShaderUniformSlot& slot,
                    const ShaderMetadata* metadata,
                    BufferView view) override {
    return native_.BindResource(stage, type, slot, metadata, std::move(view));
  }
  bool BindResource(ShaderStage stage,
                    DescriptorType type,
                    const SampledImageSlot& slot,
                    const ShaderMetadata* metadata,
                    std::shared_ptr<const Texture> texture,
                    raw_ptr<const Sampler> sampler) override {
    return native_.BindResource(stage, type, slot, metadata, std::move(texture),
                                sampler);
  }
  fml::Status Draw() override {
    if (!valid_ || !BindAvioSample4Clip(renderer_, native_, clip_, {})) {
      return {fml::StatusCode::kCancelled,
              "Fixed fringe composite unavailable"};
    }
    return native_.Draw();
  }

 protected:
  void OnSetLabel(std::string_view label) override { native_.SetLabel(label); }
  bool OnEncodeCommands(const Context&) const override { return false; }

 private:
  const ContentContext& renderer_;
  RenderPass& native_;
  const AvioSample4ClipDescriptor& clip_;
  bool valid_ = true;
};

}  // namespace

bool CoverageTiledRenderPass::Replay(RenderPass& pass,
                                     IRect raster_rect,
                                     bool translate_to_raster) const {
  for (const auto& packet : EncodingPackets()) {
    if (!ReplayPacket(pass, packet, raster_rect, translate_to_raster)) {
      return false;
    }
  }
  return true;
}

bool CoverageTiledRenderPass::ReplayPacket(
    RenderPass& pass,
    const DrawPacket& packet,
    IRect raster_rect,
    bool translate_to_raster,
    std::optional<AvioCoveragePipelineVariant> variant,
    const AvioSample4ClipDescriptor* clip_override) const {
  const auto origin = translate_to_raster ? raster_rect.GetOrigin() : IPoint{};
  auto clipped = GetPacketScissor(packet, raster_rect);
  if (!clipped) {
    return true;
  }
  auto local_scissor = clipped->Shift(-origin);
  pass.SetScissor(
      IRect32::MakeLTRB(static_cast<int32_t>(local_scissor.GetLeft()),
                        static_cast<int32_t>(local_scissor.GetTop()),
                        static_cast<int32_t>(local_scissor.GetRight()),
                        static_cast<int32_t>(local_scissor.GetBottom())));
  auto viewport = packet.viewport.value_or(
      Viewport{.rect = Rect::MakeSize(GetRenderTargetSize())});
  viewport.rect = viewport.rect.Shift(-Point(origin));
  pass.SetViewport(viewport);
  std::shared_ptr<Pipeline<PipelineDescriptor>> continuous_pipeline;
  if (variant) {
    auto replacement =
        renderer_.GetAvioPipelineVariant(packet.pipeline_owner, *variant);
    if (!replacement ||
        (replacement->GetDescriptor().GetColorAttachmentDescriptor(0)->format !=
         pass.GetRenderTargetPixelFormat())) {
      return false;
    }
    pass.SetPipeline(replacement);
    const auto* clip =
        clip_override ? clip_override : packet.sample4_clip.get();
    if (*variant != AvioCoveragePipelineVariant::kDirect1x &&
        (!clip || !BindAvioSample4Clip(renderer_, pass, *clip, origin))) {
      return false;
    }
  } else if (packet.continuous_expression_buffer) {
    const auto& descriptor = packet.pipeline->GetDescriptor();
    const auto* colour = descriptor.GetColorAttachmentDescriptor(0);
    if (!colour || !region_ ||
        static_cast<int>(colour->src_color_blend_factor) > 10 ||
        static_cast<int>(colour->dst_color_blend_factor) > 10 ||
        static_cast<int>(colour->src_alpha_blend_factor) > 10 ||
        static_cast<int>(colour->dst_alpha_blend_factor) > 10 ||
        static_cast<int>(colour->color_blend_op) >
            static_cast<int>(BlendOperation::kReverseSubtract) ||
        static_cast<int>(colour->alpha_blend_op) >
            static_cast<int>(BlendOperation::kReverseSubtract)) {
      return false;
    }
    continuous_pipeline = renderer_.GetAvioPipelineVariant(
        packet.pipeline_owner, AvioCoveragePipelineVariant::kContinuous);
    auto prefix = region_->GetContinuousDestinationPrefix();
    if (!continuous_pipeline || !prefix ||
        !renderer_.GetAvioContinuousDataBuffer()) {
      return false;
    }
    const ShaderUniformSlot control_slot{"AvioContinuousControl", 0, 0,
                                         kAvioContinuousControlBinding};
    const ShaderUniformSlot expression_slot{"AvioContinuousExpression", 0, 0,
                                            kAvioContinuousUniformBinding};
    const SampledImageSlot prefix_slot{"avio_destination", 0, 0,
                                       kAvioContinuousDestinationBinding};
    const auto control = MakeAvioContinuousControl(
        *colour, packet.continuous_clip ? packet.continuous_clip->count : 0,
        origin,
        packet.continuous_geometry_primitive
            ? &*packet.continuous_geometry_primitive
            : nullptr);
    auto sampler = renderer_.GetContext()->GetSamplerLibrary()->GetSampler(
        PixelPrefixSampler());
    if (!sampler ||
        !pass.BindResource(
            ShaderStage::kFragment, DescriptorType::kStorageBuffer,
            control_slot, nullptr,
            renderer_.GetAvioContinuousDataBuffer()->Emplace(
                &control, sizeof(control),
                std::max(alignof(AvioContinuousControl),
                         renderer_.GetDeviceCapabilities()
                             .GetMinimumStorageBufferAlignment()))) ||
        !pass.BindResource(ShaderStage::kFragment,
                           DescriptorType::kStorageBuffer, expression_slot,
                           nullptr, packet.continuous_expression_buffer) ||
        !pass.BindResource(ShaderStage::kFragment,
                           DescriptorType::kSampledImage, prefix_slot, nullptr,
                           prefix, sampler)) {
      return false;
    }
    pass.SetPipeline(continuous_pipeline);
  } else {
    pass.SetPipeline(packet.pipeline);
  }
  pass.SetCommandLabel(packet.label);
  pass.SetStencilReference(packet.stencil_reference);
  pass.SetBaseVertex(packet.base_vertex);
  pass.SetElementCount(packet.element_count);
  pass.SetInstanceCount(packet.instance_count);
  if (!pass.SetVertexBuffer(packet.vertices.data(), packet.vertices.size()) ||
      !pass.SetIndexBuffer(packet.indices, packet.index_type)) {
    return false;
  }
  for (const auto& binding : packet.bindings) {
    const bool bound = std::visit(
        [&pass](const auto& value) {
          using T = std::decay_t<decltype(value)>;
          auto slot = value.slot;
          slot.name = value.slot_name ? value.slot_name->c_str() : nullptr;
          if constexpr (std::is_same_v<T, BufferBinding>) {
            return pass.BindResource(value.stage, value.type, slot,
                                     value.metadata, value.view);
          } else {
            return pass.BindResource(value.stage, value.type, slot,
                                     value.metadata, value.texture,
                                     value.sampler);
          }
        },
        binding);
    if (!bound) {
      return false;
    }
  }
  if (!pass.Draw().ok()) {
    return false;
  }
  return true;
}

bool CoverageTiledRenderPass::EncodeTileReplay(std::shared_ptr<RenderPass> pass,
                                               IRect raster_rect) const {
  for (const auto& packet : EncodingPackets()) {
    if (!GetPacketScissor(packet, raster_rect)) {
      continue;
    }
    if (packet.continuous_expression_buffer) {
      // Finish the exact painter prefix before sampling it. Both source and
      // destination retain four native sample lanes; this is neither resolve
      // sampling nor framebuffer feedback, and barriers are outside passes.
      if (!pass->EncodeCommands()) {
        return false;
      }
      const auto prefix = region_->GetContinuousDestinationPrefix();
      auto blit = command_buffer_->CreateBlitPass();
      if (!prefix || !blit ||
          !blit->AddCopy(island_.GetColorAttachment(0).texture, prefix) ||
          !blit->ConvertTextureToShaderRead(prefix) ||
          !blit->EncodeCommands()) {
        return false;
      }
      auto resumed = island_;
      auto colour = resumed.GetColorAttachment(0);
      colour.load_action = LoadAction::kLoad;
      colour.store_action = StoreAction::kStoreAndMultisampleResolve;
      resumed.SetColorAttachment(colour, 0);
      auto depth = resumed.GetDepthAttachment().value();
      depth.load_action = LoadAction::kLoad;
      depth.store_action = StoreAction::kStore;
      resumed.SetDepthAttachment(depth);
      auto stencil = resumed.GetStencilAttachment().value();
      stencil.load_action = LoadAction::kLoad;
      stencil.store_action = StoreAction::kStore;
      resumed.SetStencilAttachment(stencil);
      pass = command_buffer_->CreateRenderPass(resumed);
      if (!pass) {
        return false;
      }
    }
    if (!ReplayPacket(*pass, packet, raster_rect, true)) {
      return false;
    }
  }
  return pass->EncodeCommands();
}

std::optional<IRect> CoverageTiledRenderPass::GetPacketScissor(
    const DrawPacket& packet,
    IRect raster_rect) const {
  auto parent_scissor = IRect::MakeSize(GetRenderTargetSize());
  if (packet.scissor) {
    parent_scissor = IRect::MakeLTRB(
        packet.scissor->GetLeft(), packet.scissor->GetTop(),
        packet.scissor->GetRight(), packet.scissor->GetBottom());
  }
  auto clipped = parent_scissor.Intersection(raster_rect);
  if (!clipped) {
    return std::nullopt;
  }
  const auto* colour =
      packet.pipeline->GetDescriptor().GetColorAttachmentDescriptor(0);
  // Depth/stencil-only packets include inverted clips and clip restores.
  // Their writes may affect every later colour draw in this tile, even when
  // the clipped shape's own geometric bounds do not intersect it.
  if (packet.coverage && colour &&
      colour->write_mask != ColorWriteMaskBits::kNone &&
      !packet.coverage->Expand(1).IntersectsWithRect(Rect::Make(*clipped))) {
    return std::nullopt;
  }
  return clipped;
}

bool CoverageTiledRenderPass::PreparePacketTextures(BlitPass& pass,
                                                    IRect raster_rect) const {
  for (const auto& packet : EncodingPackets()) {
    if (!GetPacketScissor(packet, raster_rect)) {
      continue;
    }
    for (const auto& binding : packet.bindings) {
      const auto* texture = std::get_if<TextureBinding>(&binding);
      // Texture content is immutable to the draw, while backend image layout
      // and external ownership are mutable command-buffer custody metadata.
      if (texture && !pass.ConvertTextureToShaderRead(
                         std::const_pointer_cast<Texture>(texture->texture))) {
        return false;
      }
    }
  }
  return true;
}

std::optional<Rect> CoverageTiledRenderPass::GetColourCoverage(
    IRect writable) const {
  std::optional<Rect> coverage;
  const auto writable_rect = Rect::Make(writable);
  for (const auto& packet : EncodingPackets()) {
    const auto* colour =
        packet.pipeline->GetDescriptor().GetColorAttachmentDescriptor(0);
    if (colour && colour->write_mask == ColorWriteMaskBits::kNone) {
      continue;
    }
    auto bounds = packet.coverage ? packet.coverage->Expand(1) : writable_rect;
    if (packet.scissor) {
      bounds = bounds.IntersectionOrEmpty(Rect::Make(*packet.scissor));
    }
    auto clipped = bounds.Intersection(writable_rect);
    if (clipped) {
      coverage = coverage ? coverage->Union(*clipped) : clipped;
    }
  }
  return coverage;
}

bool CoverageTiledRenderPass::CopyTile(const std::shared_ptr<Texture>& resolve,
                                       const std::shared_ptr<Texture>& parent,
                                       IRect local_content,
                                       IRect content) const {
  if (island_.GetRenderTargetPixelFormat() ==
      parent_.GetRenderTargetPixelFormat()) {
    auto copy = command_buffer_->CreateBlitPass();
    return copy &&
           copy->AddCopy(resolve, parent, local_content, content.GetOrigin(),
                         "Coverage tile resolve") &&
           copy->EncodeCommands();
  }
  // A raw texture copy does not swizzle RGBA/BGRA channel storage. A nearest,
  // unit-opacity kSrc draw preserves premultiplied colour and alpha through
  // the two declared UNORM formats without applying coverage a second time.
  auto prepare_copy = command_buffer_->CreateBlitPass();
  if (!prepare_copy || !prepare_copy->ConvertTextureToShaderRead(resolve) ||
      !prepare_copy->EncodeCommands()) {
    return false;
  }
  auto target = parent_;
  auto colour = target.GetColorAttachment(0);
  colour.load_action = LoadAction::kLoad;
  colour.store_action = StoreAction::kStore;
  target.SetColorAttachment(colour, 0);
  auto pass = command_buffer_->CreateRenderPass(target);
  if (!pass) {
    return false;
  }
  TextureContents contents;
  contents.SetDestinationRect(Rect::Make(content));
  contents.SetTexture(resolve);
  contents.SetSourceRect(Rect::Make(local_content));
  contents.SetStencilEnabled(false);
  contents.SetSamplerDescriptor(PixelPrefixSampler());
  Entity entity;
  entity.SetBlendMode(BlendMode::kSrc);
  return contents.Render(renderer_, entity, *pass) && pass->EncodeCommands();
}

bool CoverageTiledRenderPass::ReplayCertifiedColour(
    RenderPass& pass,
    IRect raster,
    bool translate,
    AvioCoveragePipelineVariant variant,
    const AvioSample4ClipDescriptor* clip) const {
  for (const auto& packet : EncodingPackets()) {
    const auto* colour =
        packet.pipeline->GetDescriptor().GetColorAttachmentDescriptor(0);
    // A complete certified expression replaces all clip and clip-restore
    // writes. Source geometry has separately been proved full on this segment.
    if (packet.clip_operation || !colour ||
        colour->write_mask == ColorWriteMaskBits::kNone) {
      continue;
    }
    if (!ReplayPacket(pass, packet, raster, translate, variant, clip)) {
      return false;
    }
  }
  return true;
}

bool CoverageTiledRenderPass::CompositeFringeLayer(
    const std::shared_ptr<Texture>& source,
    IRect local,
    IRect destination,
    const AvioSample4ClipDescriptor& clip) const {
  auto prepare = command_buffer_->CreateBlitPass();
  if (!prepare || !prepare->ConvertTextureToShaderRead(source) ||
      !prepare->EncodeCommands()) {
    return false;
  }
  auto target = parent_;
  auto colour = target.GetColorAttachment(0);
  colour.load_action = LoadAction::kLoad;
  colour.store_action = StoreAction::kStore;
  target.SetColorAttachment(colour, 0);
  auto pass = command_buffer_->CreateRenderPass(target);
  if (!pass) {
    return false;
  }
  Sample4LayerPass masked(renderer_, *pass, clip);
  TextureContents contents;
  contents.SetDestinationRect(Rect::Make(destination));
  contents.SetTexture(source);
  contents.SetSourceRect(Rect::Make(local));
  contents.SetStencilEnabled(false);
  contents.SetSamplerDescriptor(PixelPrefixSampler());
  Entity entity;
  entity.SetBlendMode(BlendMode::kSrcOver);
  return contents.Render(renderer_, entity, masked) && masked.IsValid() &&
         pass->EncodeCommands();
}

std::optional<bool> CoverageTiledRenderPass::EncodeCertifiedClip(
    IRect writable,
    bool validate_only) const {
  // Decline before any colour write unless the entire painter segment has a
  // complete immutable declaration identity. Draw ordinals and rounded bounds
  // are not clip identities, and a resolved prefix cannot recover prior lanes.
  if (!region_ || !region_->CachesNativeMasks()) {
    return std::nullopt;
  }
  std::shared_ptr<const AvioSample4ClipDescriptor> clip;
  size_t colour_count = 0;
  bool all_src_over = true;
  bool all_sources_encoded = true;
  AvioSample4OpaqueGroupProof opaque_group;
  for (const auto& packet : EncodingPackets()) {
    const auto& pipeline = packet.pipeline->GetDescriptor();
    const auto* colour = pipeline.GetColorAttachmentDescriptor(0);
    if (packet.clip_operation || !colour ||
        colour->write_mask == ColorWriteMaskBits::kNone) {
      continue;
    }
    const auto& descriptor = packet.sample4_clip;
    auto fragment = pipeline.GetEntrypointForStage(ShaderStage::kFragment);
    const auto& proof = packet.sample4_source_proof;
    const bool source_proven =
        descriptor && proof && proof->Matches(descriptor->segment_token);
    const bool solid =
        fragment && fragment->GetName() == "solid_fill_fragment_main";
    const bool captured_texture =
        fragment && fragment->GetName() == "texture_fill_fragment_main" &&
        source_proven && proof->captured_backdrop;
    if (!packet.sample4_clip_complete || !descriptor ||
        !descriptor->segment_token ||
        !descriptor->no_external_fringe_correlation ||
        (!source_proven && !descriptor->all_draws_full_clip_geometry) ||
        packet.continuous_expression_buffer || (!solid && !captured_texture) ||
        (clip && descriptor.get() != clip.get()) ||
        !renderer_.GetAvioPipelineVariant(
            packet.pipeline_owner, AvioCoveragePipelineVariant::kDirect1x)) {
      return std::nullopt;
    }
    const bool opaque = source_proven ? proof->source_is_opaque
                                      : descriptor->all_sources_opaque;
    const bool src_over = IsSrcOverSource(*colour, opaque);
    all_src_over = all_src_over && src_over;
    all_sources_encoded = all_sources_encoded && solid && source_proven &&
                          proof->source_is_encoded && opaque;
    opaque_group.Add(src_over, opaque);
    const auto direct = renderer_.GetAvioPipelineVariant(
        packet.pipeline_owner, AvioCoveragePipelineVariant::kDirect1x);
    if (direct->GetDescriptor().GetColorAttachmentDescriptor(0)->format !=
        parent_.GetRenderTargetPixelFormat()) {
      return std::nullopt;
    }
    clip = descriptor;
    colour_count++;
  }
  if (!clip || !colour_count) {
    return std::nullopt;
  }

  // One uniform full-geometry SrcOver draw resolves the complete joint clip
  // predicate once. Its own alpha remains source alpha, never a second mask.
  // A translucent covered lane is quantized before native resolve. Moving
  // that rounding after source scaling can change even a single edge byte.
  const bool per_draw =
      colour_count == 1 && all_src_over && all_sources_encoded;
  if (per_draw && MakeAvioSample4ClipUniform(*clip, {})) {
    for (const auto& packet : EncodingPackets()) {
      const auto* colour =
          packet.pipeline->GetDescriptor().GetColorAttachmentDescriptor(0);
      if (!packet.clip_operation && colour &&
          colour->write_mask != ColorWriteMaskBits::kNone &&
          !renderer_.GetAvioPipelineVariant(
              packet.pipeline_owner,
              AvioCoveragePipelineVariant::kJointSample4)) {
        return std::nullopt;
      }
    }
    if (validate_only)
      return true;
    auto prepare = command_buffer_->CreateBlitPass();
    if (!prepare || !PreparePacketTextures(*prepare, writable)) {
      return false;
    }
    for (size_t i = 0; i < clip->count; i++) {
      if (clip->nodes[i].mask && !prepare->ConvertTextureToShaderRead(
                                     clip->nodes[i]
                                         .mask->lease->GetRenderTarget()
                                         .GetColorAttachment(0)
                                         .texture)) {
        return false;
      }
    }
    if (!prepare->ConvertTextureToShaderRead(
            region_->GetCoverageAtlasTarget().GetColorAttachment(0).texture) ||
        !prepare->EncodeCommands()) {
      return false;
    }
    auto target = parent_;
    auto colour = target.GetColorAttachment(0);
    colour.load_action = LoadAction::kLoad;
    colour.store_action = StoreAction::kStore;
    target.SetColorAttachment(colour, 0);
    auto pass = command_buffer_->CreateRenderPass(target);
    return pass &&
           ReplayCertifiedColour(*pass, writable, false,
                                 AvioCoveragePipelineVariant::kJointSample4) &&
           pass->EncodeCommands();
  }

  auto fringe = clip->GetFringePlan(writable);
  if (!fringe) {
    return std::nullopt;
  }
  // Shading into the standing UNORM attachment materializes the original
  // per-lane colour rounding. Opaque alone cannot certify a raw shader
  // coefficient shortcut: its RGB may still be a non-encoded value.
  const bool fringe_layer = all_src_over && opaque_group.IsValid();
  // Every required source variant is warmed before frame work. A missing
  // optional strategy declines without writing; it must never compile here.
  for (const auto& packet : EncodingPackets()) {
    const auto* colour =
        packet.pipeline->GetDescriptor().GetColorAttachmentDescriptor(0);
    if (packet.clip_operation || !colour ||
        colour->write_mask == ColorWriteMaskBits::kNone)
      continue;
    if (per_draw) {
      if (!renderer_.GetAvioPipelineVariant(
              packet.pipeline_owner,
              AvioCoveragePipelineVariant::kJointSample4)) {
        return std::nullopt;
      }
    } else if (!fringe_layer) {
      if (!renderer_.GetAvioPipelineVariant(
              packet.pipeline_owner,
              AvioCoveragePipelineVariant::kInteriorSample4) ||
          !renderer_.GetAvioPipelineVariant(
              packet.pipeline_owner,
              AvioCoveragePipelineVariant::kFringeSample4)) {
        return std::nullopt;
      }
    }
  }
  if (validate_only)
    return true;
  auto prepare_masks = [&](const AvioSample4ClipDescriptor& descriptor) {
    auto prepare = command_buffer_->CreateBlitPass();
    if (!prepare || !PreparePacketTextures(*prepare, writable) ||
        !prepare->ConvertTextureToShaderRead(
            region_->GetCoverageAtlasTarget().GetColorAttachment(0).texture)) {
      return false;
    }
    if (!descriptor.ForEachNode([&](const auto& node) {
          return !node.mask || prepare->ConvertTextureToShaderRead(
                                   node.mask->lease->GetRenderTarget()
                                       .GetColorAttachment(0)
                                       .texture);
        })) {
      return false;
    }
    return prepare->EncodeCommands();
  };
  auto parent_pass = [&]() {
    auto target = parent_;
    auto colour = target.GetColorAttachment(0);
    colour.load_action = LoadAction::kLoad;
    colour.store_action = StoreAction::kStore;
    target.SetColorAttachment(colour, 0);
    return command_buffer_->CreateRenderPass(target);
  };
  if (!prepare_masks(*clip))
    return false;
  for (size_t i = 0; i < fringe->interior_count; ++i) {
    auto pass = parent_pass();
    if (!pass ||
        !ReplayCertifiedColour(*pass, fringe->interior_pixels[i], false,
                               AvioCoveragePipelineVariant::kDirect1x) ||
        !pass->EncodeCommands()) {
      return false;
    }
  }
  for (size_t i = 0; i < fringe->fringe_count; ++i) {
    auto tile_size = island_.GetRenderTargetSize();
    if (!MakeAvioSample4ClipUniform(*clip, {})) {
      const auto mask_size =
          region_->GetClipMaskScratchTarget().GetRenderTargetSize();
      tile_size = ISize(std::min(tile_size.width, mask_size.width),
                        std::min(tile_size.height, mask_size.height));
    }
    auto plan = CoverageAtlas::PlanTiles(Rect::Make(fringe->fringe_pixels[i]),
                                         writable, tile_size);
    if (!plan)
      return false;
    while (auto tile = plan->Next()) {
      // A deep/native recipe is flattened only for the next consumed tile.
      // Its logical claim is released after encode, while Vulkan retains the
      // physical image independently. Retaining all tile leases until submit
      // would exhaust a fixed atlas on a large desktop in one command buffer.
      AvioSample4ClipDescriptor dynamic_clip;
      const AvioSample4ClipDescriptor* combined = clip.get();
      if (!MakeAvioSample4ClipUniform(*clip, {})) {
        auto flattened = FlattenClipTile(renderer_, command_buffer_, clip,
                                         tile->raster_rect);
        if (flattened.status == PreparedFillMaskStatus::kEmpty)
          continue;
        if (flattened.status != PreparedFillMaskStatus::kPrepared ||
            !flattened.mask ||
            !dynamic_clip.AppendMask(*flattened.mask,
                                     ClipOperation::kIntersect)) {
          // kDeferred is an explicit bounded-resource refusal, never an alpha
          // fallback. Earlier encoded work remains unsubmitted on this failure.
          return false;
        }
        combined = &dynamic_clip;
      }
      if (!prepare_masks(*combined))
        return false;
      if (per_draw) {
        auto pass = parent_pass();
        if (!pass ||
            !ReplayCertifiedColour(*pass, tile->content_rect, false,
                                   AvioCoveragePipelineVariant::kJointSample4,
                                   combined) ||
            !pass->EncodeCommands()) {
          return false;
        }
      } else if (fringe_layer) {
        // Standing distinct 1x resolve storage is safe while its native4
        // island is dormant. The opaque uniform/full-source certificate avoids
        // extra translucent UNORM painter rounding through transparent storage.
        auto source = island_.GetColorAttachment(0).resolve_texture;
        ColorAttachment attachment;
        attachment.texture = source;
        attachment.load_action = LoadAction::kClear;
        attachment.store_action = StoreAction::kStore;
        attachment.clear_color = Color::BlackTransparent();
        RenderTarget scratch;
        scratch.SetColorAttachment(attachment, 0);
        auto pass = command_buffer_->CreateRenderPass(scratch);
        if (!pass ||
            !ReplayCertifiedColour(*pass, tile->raster_rect, true,
                                   AvioCoveragePipelineVariant::kDirect1x) ||
            !pass->EncodeCommands() ||
            !CompositeFringeLayer(
                source,
                tile->content_rect.Shift(-tile->raster_rect.GetOrigin()),
                tile->content_rect, *combined)) {
          return false;
        }
      } else {
        // Unknown paths need not claim a bounding-box interior. The exact
        // unresolved mask itself selects all-lanes pixels for original 1x
        // blending, and partial pixels for original native4 painter blending.
        auto pass = parent_pass();
        if (!pass ||
            !ReplayCertifiedColour(
                *pass, tile->content_rect, false,
                AvioCoveragePipelineVariant::kInteriorSample4, combined) ||
            !pass->EncodeCommands() || !EncodeNative4Tile(*tile, combined)) {
          return false;
        }
      }
    }
  }
  return true;
}

bool CoverageTiledRenderPass::EncodeTiles() const {
  auto parent_target = parent_;
  auto parent_colour = parent_target.GetColorAttachment(0);
  // No draw is allowed to sample uninitialized parent pixels when the caller
  // declares its old contents disposable.
  if (parent_colour.load_action == LoadAction::kDontCare) {
    parent_colour.load_action = LoadAction::kClear;
  }
  parent_colour.store_action = StoreAction::kStore;
  parent_target.SetColorAttachment(parent_colour, 0);
  // Warm layer-bank attachments can be larger than their logical contents.
  // First-use clearing includes the physical padding so filtering cannot read
  // stale pixels left by the preceding lease of the same image.
  if (parent_.GetContentRect() &&
      parent_colour.load_action == LoadAction::kClear) {
    // An absent render area also permits a fresh undefined-layout image;
    // Vulkan deliberately rejects partial repaint of unknown contents.
    parent_target = RenderTarget{};
    parent_target.SetColorAttachment(parent_colour, 0);
  }
  auto initialization = command_buffer_->CreateRenderPass(parent_target);
  if (!initialization || !initialization->EncodeCommands()) {
    return false;
  }
  const auto parent_texture = parent_colour.texture;
  const auto parent_bounds = IRect::MakeSize(GetRenderTargetSize());
  const auto writable =
      parent_.GetRenderArea()
          ? parent_bounds.IntersectionOrEmpty(parent_.GetRenderArea().value())
          : parent_bounds;
  if (writable.IsEmpty() || packets_.empty()) {
    return true;
  }
  // A certified declaration is an actual painter segment, not necessarily
  // the entire root pass. Prefix/tail ranges keep their original clip and own
  // winding packets. Resolve boundaries are admitted only where the frozen
  // classifier proved no external lane correlation in either time direction.
  for (const auto& packet : packets_) {
    if (!packet.sample4_clip_complete) {
      return EncodeNativeRange(writable);
    }
  }
  size_t first = 0;
  size_t scan = 0;
  std::shared_ptr<const AvioSample4ClipDescriptor> start_clip;
  while (scan < packets_.size()) {
    const auto& candidate = packets_[scan];
    const auto* colour =
        candidate.pipeline->GetDescriptor().GetColorAttachmentDescriptor(0);
    const auto clip = candidate.sample4_clip;
    if (candidate.clip_operation || !colour ||
        colour->write_mask == ColorWriteMaskBits::kNone || !clip ||
        !clip->no_external_fringe_correlation ||
        (!clip->all_draws_full_clip_geometry &&
         (!candidate.sample4_source_proof ||
          !candidate.sample4_source_proof->Matches(clip->segment_token)))) {
      scan++;
      continue;
    }
    size_t last = scan + 1;
    while (last < packets_.size() && packets_[last].sample4_clip == clip)
      last++;
    encoding_first_ = scan;
    encoding_last_ = last;
    const auto admitted = EncodeCertifiedClip(writable, true);
    if (!admitted || !*admitted) {
      scan = last;
      continue;
    }
    if (first != scan) {
      encoding_first_ = first;
      encoding_last_ = scan;
      native_start_clip_ = start_clip;
      if (!EncodeNativeRange(writable))
        return false;
    }
    encoding_first_ = scan;
    encoding_last_ = last;
    if (!EncodeCertifiedClip(writable).value_or(false))
      return false;
    start_clip = clip;
    first = scan = last;
  }
  encoding_first_ = first;
  encoding_last_ = packets_.size();
  native_start_clip_ = std::move(start_clip);
  return first == packets_.size() || EncodeNativeRange(writable);
}

bool CoverageTiledRenderPass::EncodeNativeRange(IRect writable) const {
  const auto coverage = GetColourCoverage(writable);
  if (!coverage) {
    return true;
  }
  auto plan = CoverageAtlas::PlanTiles(*coverage, writable,
                                       island_.GetRenderTargetSize());
  if (!plan.has_value()) {
    return false;
  }
  while (auto tile = plan->Next()) {
    if (!EncodeNative4Tile(*tile)) {
      return false;
    }
  }
  return true;
}

bool CoverageTiledRenderPass::RestoreNativeClip(RenderPass& pass,
                                                IRect raster) const {
  if (!native_start_clip_)
    return true;
  const Vector2 offset{-Scalar(raster.GetLeft()), -Scalar(raster.GetTop())};
  const auto remap =
      ClipMaskReplayTransform(native_start_clip_->logical_pass_size,
                              pass.GetRenderTargetSize(), raster.GetOrigin());
  return native_start_clip_->ForEachNode([&](const AvioSample4ClipNode& node) {
    if (node.geometry) {
      if (!node.geometry->vertex_buffer)
        return node.operation == ClipOperation::kDifference;
      auto geometry = *node.geometry;
      geometry.transform = remap * geometry.transform;
      ClipContents clip(node.GetBounds().Shift(offset), false);
      clip.SetGeometry(std::move(geometry));
      clip.SetClipOperation(node.operation);
      return clip.Render(renderer_, pass, node.clip_depth);
    }
    if (node.quad) {
      return CoverageMaskContents::RenderQuadClip(
          renderer_, pass, node.quad->Translated(offset), node.clip_depth,
          node.operation);
    }
    if (!node.mask)
      return false;
    auto mask = *node.mask;
    mask.target_rect = mask.target_rect.Shift(offset);
    mask.shape_bounds = mask.shape_bounds.Shift(offset);
    return CoverageMaskContents::RenderClip(
        renderer_, pass, std::span(&mask, 1), node.clip_depth, node.operation);
  });
}

bool CoverageTiledRenderPass::EncodeNative4Tile(
    const CoverageAtlasTile& tile,
    const AvioSample4ClipDescriptor* clip) const {
  // Copyback and imported initialization may leave the parent in another
  // layout. Acquire/transition it before beginning the island render pass;
  // image barriers cannot be inserted into that active render pass. The
  // command buffer holds external ownership until its final release.
  const auto seed_area =
      tile.raster_rect.Intersection(IRect::MakeSize(GetRenderTargetSize()));
  auto seed_texture = parent_.GetColorAttachment(0).texture;
  auto seed_source = seed_area;
  auto prepare_seed = command_buffer_->CreateBlitPass();
  if (!prepare_seed) {
    return false;
  }
  if (auto scratch = region_->GetColourSeedTexture()) {
    // A wrapped GL framebuffer is not a sampleable Texture2D. Copy its
    // logical pixel prefix into a distinct, bounded warm texture first.
    // The ordered reactor retains both resources through the operation.
    if (seed_area) {
      auto local = seed_area->Shift(-tile.raster_rect.GetOrigin());
      if (!prepare_seed->AddCopy(parent_.GetColorAttachment(0).texture, scratch,
                                 *seed_area, local.GetOrigin(),
                                 "Coverage GL prefix")) {
        return false;
      }
      seed_source = local;
    }
    seed_texture = std::move(scratch);
  }
  if (native_start_clip_ &&
      !native_start_clip_->ForEachNode([&](const auto& node) {
        return !node.mask || prepare_seed->ConvertTextureToShaderRead(
                                 node.mask->lease->GetRenderTarget()
                                     .GetColorAttachment(0)
                                     .texture);
      })) {
    return false;
  }
  if (!prepare_seed->ConvertTextureToShaderRead(seed_texture) ||
      !PreparePacketTextures(*prepare_seed, tile.raster_rect) ||
      !prepare_seed->EncodeCommands()) {
    return false;
  }
  auto tile_target = island_;
  const bool preserve_native_prefix =
      region_->GetContinuousDestinationPrefix() != nullptr;
  auto colour = tile_target.GetColorAttachment(0);
  colour.load_action = LoadAction::kClear;
  colour.store_action = preserve_native_prefix
                            ? StoreAction::kStoreAndMultisampleResolve
                            : StoreAction::kMultisampleResolve;
  colour.clear_color = Color::BlackTransparent();
  tile_target.SetColorAttachment(colour, 0);
  auto depth = tile_target.GetDepthAttachment().value();
  depth.load_action = LoadAction::kClear;
  depth.store_action =
      preserve_native_prefix ? StoreAction::kStore : StoreAction::kDontCare;
  depth.clear_depth = 0;
  tile_target.SetDepthAttachment(depth);
  auto stencil = tile_target.GetStencilAttachment().value();
  stencil.load_action = LoadAction::kClear;
  stencil.store_action =
      preserve_native_prefix ? StoreAction::kStore : StoreAction::kDontCare;
  stencil.clear_stencil = 0;
  tile_target.SetStencilAttachment(stencil);
  auto pass = command_buffer_->CreateRenderPass(tile_target);
  if (!pass) {
    return false;
  }
  pass->SetLabel(label_.empty() ? "Coverage colour island" : label_);
  if (seed_area.has_value()) {
    const auto destination = seed_area->Shift(-tile.raster_rect.GetOrigin());
    TextureContents seed;
    seed.SetDestinationRect(Rect::Make(destination));
    seed.SetTexture(seed_texture);
    seed.SetSourceRect(Rect::Make(seed_source.value()));
    seed.SetStencilEnabled(false);
    seed.SetSamplerDescriptor(PixelPrefixSampler());
    Entity seed_entity;
    seed_entity.SetBlendMode(BlendMode::kSrc);
    seed_entity.SetClipDepth(0);
    // The seed writes no depth: ordinary draws and strokes must still pass
    // their legacy GreaterEqual/Greater tests against clear depth zero.
    if (!seed.Render(renderer_, seed_entity, *pass)) {
      return false;
    }
  }
  if (clip) {
    if (!ReplayCertifiedColour(*pass, tile.raster_rect, true,
                               AvioCoveragePipelineVariant::kFringeSample4,
                               clip) ||
        !pass->EncodeCommands()) {
      return false;
    }
  } else if (!RestoreNativeClip(*pass, tile.raster_rect) ||
             !EncodeTileReplay(std::move(pass), tile.raster_rect)) {
    return false;
  }
  const auto local_content =
      tile.content_rect.Shift(-tile.raster_rect.GetOrigin());
  if (!CompositeTile(colour.resolve_texture,
                     parent_.GetColorAttachment(0).texture, local_content,
                     tile.content_rect)) {
    return false;
  }
  return true;
}

bool CoverageTiledRenderPass::CompositeTile(
    const std::shared_ptr<Texture>& resolve,
    const std::shared_ptr<Texture>& parent,
    IRect local_content,
    IRect content) const {
  if (!CopyTile(resolve, parent, local_content, content)) {
    return false;
  }
  region_->RecordColourIslandComposite();
  return true;
}

bool CoverageTiledRenderPass::EncodeDirect1x() const {
  const auto logical_bounds = IRect::MakeSize(GetRenderTargetSize());
  const auto writable =
      parent_.GetRenderArea()
          ? logical_bounds.IntersectionOrEmpty(*parent_.GetRenderArea())
          : logical_bounds;
  // A direct 1x pass does not need an island, seed, resolve, or copyback.
  // Its inputs still need ownership/layout acquisition outside an active
  // render pass; BindResource cannot safely insert these barriers itself.
  if (!packets_.empty()) {
    auto prepare = command_buffer_->CreateBlitPass();
    if (!prepare || !PreparePacketTextures(*prepare, writable) ||
        !prepare->EncodeCommands()) {
      return false;
    }
  }
  auto target = parent_;
  auto colour = target.GetColorAttachment(0);
  if (colour.load_action == LoadAction::kDontCare) {
    colour.load_action = LoadAction::kClear;
  }
  colour.store_action = StoreAction::kStore;
  if (parent_.GetContentRect() && colour.load_action == LoadAction::kClear) {
    // Clear the physical bank padding as well as its logical 1x content.
    // Replay's viewport/scissor remain logical and never paint that padding.
    target = RenderTarget{};
  }
  target.SetColorAttachment(colour, 0);
  auto pass = command_buffer_->CreateRenderPass(target);
  if (!pass) {
    return false;
  }
  pass->SetLabel(label_.empty() ? "Coverage direct 1x filter" : label_);
  return Replay(*pass, writable, false) && pass->EncodeCommands();
}

bool CoverageTiledRenderPass::OnEncodeCommands(const Context&) const {
  if (encode_attempted_) {
    return encode_succeeded_;
  }
  encode_attempted_ = true;
  encode_succeeded_ = valid_ && TransferResourceCustody() &&
                      (tiled_ ? EncodeTiles() : EncodeDirect1x());
  valid_ = encode_succeeded_;
  return encode_succeeded_;
}

}  // namespace impeller
