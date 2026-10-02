// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <limits>
#include <new>
#include <utility>
#include "fml/logging.h"
#include "impeller/entity/avio_coverage_region.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/coverage_recorder_bank.h"
#include "impeller/entity/coverage_tiled_render_pass.h"
#include "impeller/renderer/context.h"
#include "impeller/renderer/render_resource_scope.h"
namespace impeller {
namespace {
using RecorderName = CoverageRecorderString<128>;
bool ValidSlotName(const char* name) {
  return !name || std::char_traits<char>::length(name) < 128;
}
std::optional<RecorderName> CopySlotName(const char* name) {
  return name ? std::make_optional(RecorderName(name)) : std::nullopt;
}

// The supported parent has one1x colour attachment and no D/S. Inspect that
// fixed shape directly: RenderTarget's general debug validation wraps its
// multi-attachment callbacks in allocating std::function objects.
bool ValidParentTarget(const RenderTarget& target) {
  if (!target.HasColorAttachment(0) ||
      target.GetMaxColorAttachmentBindIndex() != 0 ||
      target.GetDepthAttachment() || target.GetStencilAttachment()) {
    return false;
  }
  const auto colour = target.GetColorAttachment(0);
  if (!colour.texture || !colour.texture->IsValid() ||
      colour.texture->GetTextureDescriptor().sample_count !=
          SampleCount::kCount1 ||
      colour.texture->GetTextureDescriptor().type != TextureType::kTexture2D) {
    return false;
  }
  const auto extent = target.GetRenderTargetSize();
  const auto bounds = IRect::MakeSize(extent);
  return !extent.IsEmpty() &&
         (!target.GetRenderArea() ||
          bounds.Contains(*target.GetRenderArea())) &&
         (!target.GetContentRect() ||
          bounds.Contains(*target.GetContentRect()));
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

}  // namespace

const CoverageTiledRenderPass::DrawPacket&
CoverageTiledRenderPass::PacketSequence::Iterator::operator*() const {
  return storage->packets[index].draw;
}
CoverageTiledRenderPass::PacketSequence::Iterator&
CoverageTiledRenderPass::PacketSequence::Iterator::operator++() {
  index = storage->packets[index].next;
  return *this;
}
const CoverageTiledRenderPass::DrawPacket&
CoverageTiledRenderPass::PacketSequence::operator[](size_t index) const {
  FML_CHECK(index < count);
  const auto physical_index =
      storage->ordinal_indices[index_chunks[index / 64]][index % 64];
  return storage->packets[physical_index].draw;
}

std::shared_ptr<CoverageRecorderStorage>
CoverageTiledRenderPass::CreateStorage() {
  return IsAvioRasterFrameActive()
             ? nullptr
             : std::make_shared<CoverageRecorderStorage>();
}

std::shared_ptr<CoverageTiledRenderPass> CoverageTiledRenderPass::Claim(
    const ContentContext& renderer,
    const RenderTarget& parent,
    RenderTarget island,
    std::shared_ptr<AvioCoverageRegion> region,
    std::shared_ptr<CommandBuffer> command_buffer,
    bool tiled) {
  auto storage = renderer.GetCoverageRecorderStorage();
  if (!storage) {
    return nullptr;
  }
  auto index = storage->ReserveControl();
  if (!index) {
    return nullptr;
  }
  return std::allocate_shared<CoverageTiledRenderPass>(
      CoverageRecorderAllocator<CoverageTiledRenderPass>(storage, *index),
      renderer, parent, std::move(island), std::move(region),
      std::move(command_buffer), tiled, std::move(storage), *index);
}

std::shared_ptr<CoverageTiledRenderPass> CoverageTiledRenderPass::Make(
    const ContentContext& renderer,
    const RenderTarget& parent,
    std::shared_ptr<CommandBuffer> command_buffer) {
  auto region = renderer.GetContext()->GetAvioCoverageRegion();
  if (!region || !region->IsColourInitialized() || !command_buffer ||
      !command_buffer->IsValid() || !ValidParentTarget(parent) ||
      parent.GetSampleCount() != SampleCount::kCount1 ||
      parent.GetDepthAttachment() || parent.GetStencilAttachment() ||
      parent.GetMaxColorAttachmentBindIndex() != 0) {
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
  if (!island.HasColorAttachment(0) ||
      island.GetSampleCount() != SampleCount::kCount4 ||
      !island.GetDepthAttachment() || !island.GetStencilAttachment() ||
      !island.GetColorAttachment(0).resolve_texture ||
      !CompatibleColourFormats(island.GetRenderTargetPixelFormat(),
                               parent.GetRenderTargetPixelFormat())) {
    return nullptr;
  }
  return Claim(renderer, parent, std::move(island), std::move(region),
               std::move(command_buffer), true);
}

std::shared_ptr<CoverageTiledRenderPass> CoverageTiledRenderPass::MakeDirect1x(
    const ContentContext& renderer,
    const RenderTarget& parent,
    std::shared_ptr<CommandBuffer> command_buffer) {
  if (!command_buffer || !command_buffer->IsValid() ||
      !ValidParentTarget(parent) ||
      parent.GetSampleCount() != SampleCount::kCount1 ||
      parent.GetDepthAttachment() || parent.GetStencilAttachment() ||
      parent.GetMaxColorAttachmentBindIndex() != 0) {
    return nullptr;
  }
  const auto& colour = parent.GetColorAttachment(0);
  if (colour.resolve_texture || colour.mip_level != 0 || colour.slice != 0 ||
      (parent.GetContentRect() &&
       parent.GetContentRect()->GetOrigin() != IPoint{})) {
    return nullptr;
  }
  return Claim(renderer, parent, parent, nullptr, std::move(command_buffer),
               false);
}

CoverageTiledRenderPass::CoverageTiledRenderPass(
    const ContentContext& renderer,
    const RenderTarget& parent,
    RenderTarget island,
    std::shared_ptr<AvioCoverageRegion> region,
    std::shared_ptr<CommandBuffer> command_buffer,
    bool tiled,
    std::shared_ptr<CoverageRecorderStorage> storage,
    size_t control_slot)
    : RenderPass(renderer.GetContext(),
                 tiled ? island : parent,
                 LogicalSize(parent)),
      renderer_(renderer),
      parent_(parent),
      island_(std::move(island)),
      region_(std::move(region)),
      command_buffer_(std::move(command_buffer)),
      tiled_(tiled),
      storage_(std::move(storage)),
      control_slot_(control_slot) {
  packets_.storage = storage_.get();
  RetainResource(parent.GetResourceOwner());
}

CoverageTiledRenderPass::~CoverageTiledRenderPass() {
  storage_->ReleaseRecorder(control_slot_);
}

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
    pending_.label.AssignLabel(label);
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

void CoverageTiledRenderPass::SetAvioSample4Clip(
    std::shared_ptr<const AvioSample4ClipDescriptor> descriptor,
    bool complete) {
  if (AcceptMutation()) {
    avio_sample4_clip_ = std::move(descriptor);
    avio_sample4_clip_complete_ = complete;
  }
}

void CoverageTiledRenderPass::SetAvioClipOperation(bool clip_operation) {
  if (AcceptMutation()) {
    RenderPass::SetAvioClipOperation(clip_operation);
  }
}

void CoverageTiledRenderPass::SetAvioSample4SourceProof(
    std::optional<AvioSample4SourceProof> proof) {
  if (AcceptMutation()) {
    sample4_source_proof_ = std::move(proof);
  }
}

const AvioSample4ClipDescriptor* CoverageTiledRenderPass::GetAvioSample4Clip()
    const {
  return avio_sample4_clip_.get();
}

void CoverageTiledRenderPass::SetAvioContinuousClip(
    std::shared_ptr<const AvioContinuousClipExpression> expression) {
  if (AcceptMutation()) {
    avio_continuous_clip_ = std::move(expression);
  }
}

void CoverageTiledRenderPass::SetAvioContinuousGeometry(bool enabled) {
  if (AcceptMutation()) {
    pending_.continuous_geometry = enabled;
  }
}

void CoverageTiledRenderPass::SetAvioContinuousGeometryPrimitive(
    std::optional<AvioContinuousPrimitive> primitive) {
  if (AcceptMutation()) {
    pending_.continuous_geometry_primitive = std::move(primitive);
  }
}

void CoverageTiledRenderPass::RetainResource(std::shared_ptr<void> owner) {
  if (AcceptMutation() && owner) {
    const auto owners = logical_owners_.span();
    if (std::find(owners.begin(), owners.end(), owner) == owners.end()) {
      valid_ = logical_owners_.push_back(std::move(owner));
    }
  }
}

bool CoverageTiledRenderPass::TransferResourceCustody() const {
  for (const auto& owner : logical_owners_.span()) {
    if (!command_buffer_->TryRetainResource(owner)) {
      return false;
    }
  }
  return command_buffer_->HasValidResourceOwners();
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
    if (count > pending_vertices_.capacity() - pending_vertices_.size()) {
      valid_ = false;
      return false;
    }
    for (size_t i = 0; i < count; i++) {
      pending_vertices_.push_back(buffers[i]);
    }
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
  if (!pending_.element_count || !pending_.instance_count) {
    pending_ = DrawPacket{};
    sample4_source_proof_.reset();
    pending_vertices_.clear();
    pending_bindings_.clear();
    return {};
  }
  // Claim the entire immutable packet and its resource ranges before any
  // expression upload. Failed admission leaves no partially recorded draw.
  auto reservation = storage_->ReserveDraw(
      packets_.size(), pending_vertices_.size(), pending_bindings_.size());
  if (!reservation) {
    valid_ = false;
    return {fml::StatusCode::kResourceExhausted,
            "Fixed coverage recorder capacity unavailable"};
  }
  pending_.viewport = viewport_;
  pending_.scissor = scissor_;
  pending_.coverage = coverage_;
  pending_.sample4_clip = avio_sample4_clip_;
  pending_.sample4_source_proof = sample4_source_proof_;
  pending_.sample4_clip_complete = avio_sample4_clip_complete_;
  pending_.clip_operation = IsAvioClipOperation();
  pending_.continuous_clip = avio_continuous_clip_;
  const auto* colour =
      pending_.pipeline->GetDescriptor().GetColorAttachmentDescriptor(0);
  const bool continuous =
      colour && colour->write_mask != ColorWriteMaskBits::kNone &&
      (pending_.continuous_geometry || pending_.continuous_geometry_primitive ||
       (pending_.continuous_clip && !pending_.continuous_clip->IsEmpty()));
  if (continuous) {
    if (!tiled_ ||
        (pending_.continuous_clip &&
         pending_.continuous_clip->count > kAvioContinuousMaxClips)) {
      valid_ = false;
      return {fml::StatusCode::kCancelled,
              "Unsupported continuous coverage draw"};
    }
    auto found =
        std::find_if(continuous_buffers_.begin(),
                     continuous_buffers_.begin() + continuous_buffers_count_,
                     [&](const ClipBuffer& buffer) {
                       return buffer.expression == pending_.continuous_clip;
                     });
    if (found == continuous_buffers_.begin() + continuous_buffers_count_) {
      if (continuous_buffers_count_ == kMaxContinuousStates ||
          !renderer_.GetAvioContinuousDataBuffer()) {
        valid_ = false;
        return {fml::StatusCode::kCancelled,
                "Continuous state capacity exhausted"};
      }
      BufferView buffer;
      if (pending_.continuous_clip && !pending_.continuous_clip->IsEmpty()) {
        const auto& primitives = pending_.continuous_clip->primitives;
        buffer = renderer_.GetAvioContinuousDataBuffer()->Emplace(
            primitives.data(), sizeof(primitives),
            std::max(alignof(AvioContinuousPrimitive),
                     renderer_.GetDeviceCapabilities()
                         .GetMinimumStorageBufferAlignment()));
      } else {
        // No expression elements are read when count is zero. A distinct
        // tiny valid binding satisfies the descriptor without a 10 KiB upload.
        const AvioContinuousVector empty{};
        buffer = renderer_.GetAvioContinuousDataBuffer()->Emplace(
            empty.data(), sizeof(empty),
            std::max(alignof(AvioContinuousPrimitive),
                     renderer_.GetDeviceCapabilities()
                         .GetMinimumStorageBufferAlignment()));
      }
      if (!buffer) {
        valid_ = false;
        return {fml::StatusCode::kCancelled,
                "Continuous expression upload failed"};
      }
      continuous_buffers_[continuous_buffers_count_++] = {
          pending_.continuous_clip, buffer};
      pending_.continuous_expression_buffer = std::move(buffer);
    } else {
      pending_.continuous_expression_buffer = found->buffer;
    }
  }
  pending_.stencil_reference = stencil_reference_;
  reservation->Commit(packets_, std::move(pending_), pending_vertices_.span(),
                      pending_bindings_.span());
  pending_ = DrawPacket{};
  sample4_source_proof_.reset();
  pending_vertices_.clear();
  pending_bindings_.clear();
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
  if (!view || !ValidSlotName(slot.name) ||
      pending_bindings_.size() == pending_bindings_.capacity()) {
    valid_ = false;
    return false;
  }
  pending_bindings_.emplace_back(BufferBinding{stage, type, slot, metadata,
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
  if (!texture || !texture->IsValid() || !sampler ||
      !ValidSlotName(slot.name) ||
      pending_bindings_.size() == pending_bindings_.capacity()) {
    valid_ = false;
    return false;
  }
  pending_bindings_.emplace_back(
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
  if (!view || !metadata || !ValidSlotName(slot.name) ||
      pending_bindings_.size() == pending_bindings_.capacity()) {
    valid_ = false;
    return false;
  }
  std::unique_ptr<const ShaderMetadata> owner(std::move(metadata));
  const auto* pointer = owner.get();
  pending_bindings_.emplace_back(
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
  if (!texture || !texture->IsValid() || !sampler || !metadata ||
      !ValidSlotName(slot.name) ||
      pending_bindings_.size() == pending_bindings_.capacity()) {
    valid_ = false;
    return false;
  }
  std::unique_ptr<const ShaderMetadata> owner(std::move(metadata));
  const auto* pointer = owner.get();
  pending_bindings_.emplace_back(
      TextureBinding{stage, type, slot, pointer, std::move(owner),
                     std::move(texture), sampler, CopySlotName(slot.name)});
  return true;
}

void CoverageTiledRenderPass::OnSetLabel(std::string_view label) {
  if (AcceptMutation()) {
    label_.AssignLabel(label);
  }
}

}  // namespace impeller
