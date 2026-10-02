// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/coverage_tiled_render_pass.h"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

#include "impeller/entity/avio_coverage_region.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/contents/coverage_atlas.h"
#include "impeller/entity/contents/texture_contents.h"
#include "impeller/entity/entity.h"
#include "impeller/renderer/blit_pass.h"
#include "impeller/renderer/context.h"

namespace impeller {
namespace {

std::optional<std::string> CopySlotName(const char* name) {
  return name ? std::make_optional(std::string(name)) : std::nullopt;
}

ISize LogicalSize(const RenderTarget& target) {
  return target.GetContentRect() ? target.GetContentRect()->GetSize()
                                 : target.GetRenderTargetSize();
}

bool CompatibleColourFormats(PixelFormat a, PixelFormat b) {
  if (a == b) {
    return true;
  }
  // Sampling and rendering swizzle channels through their declared formats.
  // Other mismatches may change precision or transfer encoding and are not
  // interchangeable with this fixed UNORM colour island.
  return (a == PixelFormat::kB8G8R8A8UNormInt &&
          b == PixelFormat::kR8G8B8A8UNormInt) ||
         (b == PixelFormat::kB8G8R8A8UNormInt &&
          a == PixelFormat::kR8G8B8A8UNormInt);
}

SamplerDescriptor PixelPrefixSampler() {
  SamplerDescriptor sampler;
  sampler.min_filter = MinMagFilter::kNearest;
  sampler.mag_filter = MinMagFilter::kNearest;
  sampler.mip_filter = MipFilter::kBase;
  return sampler;
}

}  // namespace

std::shared_ptr<CoverageTiledRenderPass> CoverageTiledRenderPass::Make(
    const ContentContext& renderer,
    const RenderTarget& parent,
    std::shared_ptr<CommandBuffer> command_buffer) {
  auto region = renderer.GetContext()->GetAvioCoverageRegion();
  if (!region || !region->IsColourInitialized() || !command_buffer ||
      !command_buffer->IsValid() || !parent.IsValid() ||
      parent.GetSampleCount() != SampleCount::kCount1 ||
      parent.GetDepthAttachment() || parent.GetStencilAttachment()) {
    return nullptr;
  }
  const auto& parent_colour = parent.GetColorAttachment(0);
  // BlitPass::AddCopy targets the base subresource. Other subresources need
  // an explicit API rather than silently copying to mip/layer zero.
  if (parent_colour.resolve_texture || parent_colour.mip_level != 0 ||
      parent_colour.slice != 0 ||
      (parent.GetContentRect() &&
       parent.GetContentRect()->GetOrigin() != IPoint{})) {
    return nullptr;
  }
  auto island = region->GetColourIslandTarget();
  if (!island.IsValid() || island.GetSampleCount() != SampleCount::kCount4 ||
      !island.GetDepthAttachment() || !island.GetStencilAttachment() ||
      !island.GetColorAttachment(0).resolve_texture ||
      !CompatibleColourFormats(island.GetRenderTargetPixelFormat(),
                               parent.GetRenderTargetPixelFormat())) {
    return nullptr;
  }
  return std::shared_ptr<CoverageTiledRenderPass>(new CoverageTiledRenderPass(
      renderer, parent, std::move(island), std::move(region),
      std::move(command_buffer)));
}

std::shared_ptr<CoverageTiledRenderPass> CoverageTiledRenderPass::MakeDirect1x(
    const ContentContext& renderer,
    const RenderTarget& parent,
    std::shared_ptr<CommandBuffer> command_buffer) {
  if (!command_buffer || !command_buffer->IsValid() || !parent.IsValid() ||
      parent.GetSampleCount() != SampleCount::kCount1 ||
      parent.GetDepthAttachment() || parent.GetStencilAttachment()) {
    return nullptr;
  }
  const auto& colour = parent.GetColorAttachment(0);
  if (colour.resolve_texture || colour.mip_level != 0 || colour.slice != 0 ||
      (parent.GetContentRect() &&
       parent.GetContentRect()->GetOrigin() != IPoint{})) {
    return nullptr;
  }
  return std::shared_ptr<CoverageTiledRenderPass>(new CoverageTiledRenderPass(
      renderer, parent, parent, nullptr, std::move(command_buffer), false));
}

CoverageTiledRenderPass::CoverageTiledRenderPass(
    const ContentContext& renderer,
    const RenderTarget& parent,
    RenderTarget island,
    std::shared_ptr<AvioCoverageRegion> region,
    std::shared_ptr<CommandBuffer> command_buffer,
    bool tiled)
    : RenderPass(renderer.GetContext(),
                 tiled ? island : parent,
                 LogicalSize(parent)),
      renderer_(renderer),
      parent_(parent),
      island_(std::move(island)),
      region_(std::move(region)),
      command_buffer_(std::move(command_buffer)),
      tiled_(tiled) {
  RenderPass::RetainResource(parent.GetResourceOwner());
  command_buffer_->RetainResource(parent.GetResourceOwner());
}

CoverageTiledRenderPass::~CoverageTiledRenderPass() = default;

bool CoverageTiledRenderPass::IsValid() const {
  return valid_ && (!encode_attempted_ || encode_succeeded_);
}

bool CoverageTiledRenderPass::AcceptMutation() {
  if (encode_attempted_) {
    valid_ = false;
  }
  return valid_;
}

void CoverageTiledRenderPass::SetPipeline(PipelineRef pipeline) {
  if (AcceptMutation()) {
    pending_.pipeline = pipeline;
    pending_.pipeline_owner = pipeline.Lock();
    if (!pending_.pipeline_owner) {
      valid_ = false;
    }
  }
}

void CoverageTiledRenderPass::SetCommandLabel(std::string_view label) {
  if (AcceptMutation()) {
    pending_.label = std::string(label);
  }
}

void CoverageTiledRenderPass::SetStencilReference(uint32_t value) {
  if (AcceptMutation()) {
    stencil_reference_ = value;
  }
}

void CoverageTiledRenderPass::SetBaseVertex(uint64_t value) {
  if (AcceptMutation()) {
    pending_.base_vertex = value;
  }
}

void CoverageTiledRenderPass::SetViewport(Viewport viewport) {
  if (AcceptMutation()) {
    viewport_ = viewport;
  }
}

void CoverageTiledRenderPass::SetScissor(IRect32 scissor) {
  if (AcceptMutation()) {
    scissor_ = scissor;
  }
}

void CoverageTiledRenderPass::SetDrawCoverage(std::optional<Rect> coverage) {
  if (AcceptMutation()) {
    coverage_ = coverage && coverage->IsFinite() ? coverage : std::nullopt;
  }
}

void CoverageTiledRenderPass::RetainResource(std::shared_ptr<void> owner) {
  if (AcceptMutation()) {
    command_buffer_->RetainResource(owner);
    RenderPass::RetainResource(std::move(owner));
  }
}

void CoverageTiledRenderPass::SetElementCount(size_t count) {
  if (AcceptMutation()) {
    if (count > std::numeric_limits<uint32_t>::max()) {
      valid_ = false;
      return;
    }
    pending_.element_count = static_cast<uint32_t>(count);
  }
}

void CoverageTiledRenderPass::SetInstanceCount(size_t count) {
  if (AcceptMutation()) {
    if (count > std::numeric_limits<uint32_t>::max()) {
      valid_ = false;
      return;
    }
    pending_.instance_count = static_cast<uint32_t>(count);
  }
}

bool CoverageTiledRenderPass::SetVertexBuffer(BufferView buffers[],
                                              size_t count) {
  if (!AcceptMutation()) {
    return false;
  }
  if ((count != 0 && buffers == nullptr) ||
      !ValidateVertexBuffers(buffers, count)) {
    valid_ = false;
    return false;
  }
  if (count != 0) {
    // Multiple binding calls before Draw append vertex streams, as in the
    // ordinary RenderPass recorder. A zero-count call is a no-op.
    pending_.vertices.insert(pending_.vertices.end(), buffers, buffers + count);
  }
  return true;
}

bool CoverageTiledRenderPass::SetIndexBuffer(BufferView indices,
                                             IndexType type) {
  if (!AcceptMutation()) {
    return false;
  }
  if (!ValidateIndexBuffer(indices, type)) {
    valid_ = false;
    return false;
  }
  pending_.indices = std::move(indices);
  pending_.index_type = type;
  return true;
}

fml::Status CoverageTiledRenderPass::Draw() {
  if (!AcceptMutation() || !pending_.pipeline ||
      !pending_.pipeline->IsValid()) {
    valid_ = false;
    return fml::Status(fml::StatusCode::kCancelled,
                       "Invalid coverage-island draw");
  }
  pending_.viewport = viewport_;
  pending_.scissor = scissor_;
  pending_.coverage = coverage_;
  pending_.stencil_reference = stencil_reference_;
  if (pending_.element_count != 0 && pending_.instance_count != 0) {
    packets_.push_back(std::move(pending_));
  }
  pending_ = DrawPacket{};
  return fml::Status();
}

bool CoverageTiledRenderPass::BindResource(ShaderStage stage,
                                           DescriptorType type,
                                           const ShaderUniformSlot& slot,
                                           const ShaderMetadata* metadata,
                                           BufferView view) {
  if (!AcceptMutation()) {
    return false;
  }
  if (!view) {
    valid_ = false;
    return false;
  }
  pending_.bindings.emplace_back(BufferBinding{stage, type, slot, metadata,
                                               nullptr, std::move(view),
                                               CopySlotName(slot.name)});
  return true;
}

bool CoverageTiledRenderPass::BindResource(
    ShaderStage stage,
    DescriptorType type,
    const SampledImageSlot& slot,
    const ShaderMetadata* metadata,
    std::shared_ptr<const Texture> texture,
    raw_ptr<const Sampler> sampler) {
  if (!AcceptMutation()) {
    return false;
  }
  if (!texture || !texture->IsValid() || !sampler) {
    valid_ = false;
    return false;
  }
  pending_.bindings.emplace_back(
      TextureBinding{stage, type, slot, metadata, nullptr, std::move(texture),
                     sampler, CopySlotName(slot.name)});
  return true;
}

bool CoverageTiledRenderPass::BindDynamicResource(
    ShaderStage stage,
    DescriptorType type,
    const ShaderUniformSlot& slot,
    std::unique_ptr<ShaderMetadata> metadata,
    BufferView view) {
  if (!AcceptMutation()) {
    return false;
  }
  if (!view || !metadata) {
    valid_ = false;
    return false;
  }
  std::shared_ptr<const ShaderMetadata> owner(std::move(metadata));
  const auto* pointer = owner.get();
  pending_.bindings.emplace_back(
      BufferBinding{stage, type, slot, pointer, std::move(owner),
                    std::move(view), CopySlotName(slot.name)});
  return true;
}

bool CoverageTiledRenderPass::BindDynamicResource(
    ShaderStage stage,
    DescriptorType type,
    const SampledImageSlot& slot,
    std::unique_ptr<ShaderMetadata> metadata,
    std::shared_ptr<const Texture> texture,
    raw_ptr<const Sampler> sampler) {
  if (!AcceptMutation()) {
    return false;
  }
  if (!texture || !texture->IsValid() || !sampler || !metadata) {
    valid_ = false;
    return false;
  }
  std::shared_ptr<const ShaderMetadata> owner(std::move(metadata));
  const auto* pointer = owner.get();
  pending_.bindings.emplace_back(
      TextureBinding{stage, type, slot, pointer, std::move(owner),
                     std::move(texture), sampler, CopySlotName(slot.name)});
  return true;
}

void CoverageTiledRenderPass::OnSetLabel(std::string_view label) {
  if (AcceptMutation()) {
    label_ = std::string(label);
  }
}

bool CoverageTiledRenderPass::Replay(RenderPass& pass,
                                     IRect raster_rect,
                                     bool translate_to_raster) const {
  const auto origin = translate_to_raster ? raster_rect.GetOrigin() : IPoint{};
  for (const auto& packet : packets_) {
    auto clipped = GetPacketScissor(packet, raster_rect);
    if (!clipped.has_value()) {
      continue;
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
    pass.SetPipeline(packet.pipeline);
    pass.SetCommandLabel(packet.label);
    pass.SetStencilReference(packet.stencil_reference);
    pass.SetBaseVertex(packet.base_vertex);
    pass.SetElementCount(packet.element_count);
    pass.SetInstanceCount(packet.instance_count);
    if (!pass.SetVertexBuffer(packet.vertices) ||
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
  }
  return true;
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
  for (const auto& packet : packets_) {
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
  for (const auto& packet : packets_) {
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
  auto contents = TextureContents::MakeRect(Rect::Make(content));
  contents->SetTexture(resolve);
  contents->SetSourceRect(Rect::Make(local_content));
  contents->SetStencilEnabled(false);
  contents->SetSamplerDescriptor(PixelPrefixSampler());
  Entity entity;
  entity.SetBlendMode(BlendMode::kSrc);
  entity.SetContents(contents);
  return contents->Render(renderer_, entity, *pass) && pass->EncodeCommands();
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
    // Copyback and imported initialization may leave the parent in another
    // layout. Acquire/transition it before beginning the island render pass;
    // image barriers cannot be inserted into that active render pass. The
    // command buffer holds external ownership until its final release.
    auto prepare_seed = command_buffer_->CreateBlitPass();
    if (!prepare_seed ||
        !prepare_seed->ConvertTextureToShaderRead(parent_texture) ||
        !PreparePacketTextures(*prepare_seed, tile->raster_rect) ||
        !prepare_seed->EncodeCommands()) {
      return false;
    }
    auto tile_target = island_;
    auto colour = tile_target.GetColorAttachment(0);
    colour.load_action = LoadAction::kClear;
    colour.store_action = StoreAction::kMultisampleResolve;
    colour.clear_color = Color::BlackTransparent();
    tile_target.SetColorAttachment(colour, 0);
    auto depth = tile_target.GetDepthAttachment().value();
    depth.load_action = LoadAction::kClear;
    depth.store_action = StoreAction::kDontCare;
    depth.clear_depth = 0;
    tile_target.SetDepthAttachment(depth);
    auto stencil = tile_target.GetStencilAttachment().value();
    stencil.load_action = LoadAction::kClear;
    stencil.store_action = StoreAction::kDontCare;
    stencil.clear_stencil = 0;
    tile_target.SetStencilAttachment(stencil);
    auto pass = command_buffer_->CreateRenderPass(tile_target);
    if (!pass) {
      return false;
    }
    pass->SetLabel(label_.empty() ? "Coverage colour island" : label_);
    const auto seed_area = tile->raster_rect.Intersection(parent_bounds);
    if (seed_area.has_value()) {
      const auto destination = seed_area->Shift(-tile->raster_rect.GetOrigin());
      auto seed = TextureContents::MakeRect(Rect::Make(destination));
      seed->SetTexture(parent_texture);
      seed->SetSourceRect(Rect::Make(seed_area.value()));
      seed->SetStencilEnabled(false);
      seed->SetSamplerDescriptor(PixelPrefixSampler());
      Entity seed_entity;
      seed_entity.SetBlendMode(BlendMode::kSrc);
      seed_entity.SetClipDepth(0);
      seed_entity.SetContents(seed);
      // The seed writes no depth: ordinary draws and strokes must still pass
      // their legacy GreaterEqual/Greater tests against clear depth zero.
      if (!seed->Render(renderer_, seed_entity, *pass)) {
        return false;
      }
    }
    if (!Replay(*pass, tile->raster_rect) || !pass->EncodeCommands()) {
      return false;
    }
    const auto local_content =
        tile->content_rect.Shift(-tile->raster_rect.GetOrigin());
    if (!CompositeTile(colour.resolve_texture, parent_texture, local_content,
                       tile->content_rect)) {
      return false;
    }
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
  encode_succeeded_ = valid_ && (tiled_ ? EncodeTiles() : EncodeDirect1x());
  valid_ = encode_succeeded_;
  return encode_succeeded_;
}

}  // namespace impeller
