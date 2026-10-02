// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_COVERAGE_TILED_RENDER_PASS_H_
#define FLUTTER_IMPELLER_ENTITY_COVERAGE_TILED_RENDER_PASS_H_

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "impeller/entity/contents/sample4_clip.h"
#include "impeller/entity/contents/sample4_clip_pipeline.h"
#include "impeller/entity/coverage_recorder_storage.h"
#include "impeller/renderer/render_pass.h"

namespace impeller {

class AvioCoverageRegion;
class BlitPass;
class ContentContext;
struct CoverageAtlasTile;
class CoverageRecorderStorage;
template <class T>
struct CoverageRecorderAllocator;

// Records a contiguous painter-order segment in logical parent coordinates.
// Encoding replays every draw through the context's fixed 4x colour island,
// then copies only the tile's writable pixels back to its 1x parent. Clip,
// fill-rule and stroke-overdraw depth/stencil remain bounded by the island.
//
// The recorder deliberately retains typed bindings: Vulkan encodes draws
// immediately and RenderPass::Command does not retain their binding slots.
// HostBuffer must not reset its frame epoch until all recorded segments have
// been encoded and submitted. A destination read is a segment barrier; a new
// segment starts from the exact resolved 1x prefix, not its old sample colours.
class CoverageTiledRenderPass final : public RenderPass {
 public:
  static std::shared_ptr<CoverageTiledRenderPass> Make(
      const ContentContext& renderer,
      const RenderTarget& parent,
      std::shared_ptr<CommandBuffer> command_buffer);

  // Filters which explicitly request no coverage AA keep their 1x/no-depth
  // pipeline state. Typed recording still prepares sampled image layouts and
  // custody before beginning the actual render pass.
  static std::shared_ptr<CoverageTiledRenderPass> MakeDirect1x(
      const ContentContext& renderer,
      const RenderTarget& parent,
      std::shared_ptr<CommandBuffer> command_buffer);

  ~CoverageTiledRenderPass() override;

  // One bounded CPU bank is created before raster work, shared by all passes
  // of this content context. No failed claim grows or allocates a replacement.
  static std::shared_ptr<CoverageRecorderStorage> CreateStorage();

  bool IsValid() const override;

  using RenderPass::SetPipeline;
  using RenderPass::SetVertexBuffer;
  void SetPipeline(PipelineRef pipeline) override;
  void SetCommandLabel(std::string_view label) override;
  void SetStencilReference(uint32_t value) override;
  void SetBaseVertex(uint64_t value) override;
  void SetViewport(Viewport viewport) override;
  void SetScissor(IRect32 scissor) override;
  void SetDrawCoverage(std::optional<Rect> coverage) override;
  void SetAvioSample4Clip(
      std::shared_ptr<const AvioSample4ClipDescriptor> descriptor,
      bool complete = true) override;
  void SetAvioClipOperation(bool enabled) override;
  void SetAvioSample4SourceProof(
      std::optional<AvioSample4SourceProof> proof) override;
  const AvioSample4ClipDescriptor* GetAvioSample4Clip() const override;
  void SetAvioContinuousClip(
      std::shared_ptr<const AvioContinuousClipExpression> expression) override;
  void SetAvioContinuousGeometry(bool enabled) override;
  void SetAvioContinuousGeometryPrimitive(
      std::optional<AvioContinuousPrimitive> primitive) override;
  void RetainResource(std::shared_ptr<void> owner) override;
  void SetElementCount(size_t count) override;
  void SetInstanceCount(size_t count) override;
  bool SetVertexBuffer(BufferView vertex_buffers[], size_t count) override;
  bool SetIndexBuffer(BufferView index_buffer, IndexType type) override;
  fml::Status Draw() override;

  bool BindResource(ShaderStage stage,
                    DescriptorType type,
                    const ShaderUniformSlot& slot,
                    const ShaderMetadata* metadata,
                    BufferView view) override;
  bool BindResource(ShaderStage stage,
                    DescriptorType type,
                    const SampledImageSlot& slot,
                    const ShaderMetadata* metadata,
                    std::shared_ptr<const Texture> texture,
                    raw_ptr<const Sampler> sampler) override;
  bool BindDynamicResource(ShaderStage stage,
                           DescriptorType type,
                           const ShaderUniformSlot& slot,
                           std::unique_ptr<ShaderMetadata> metadata,
                           BufferView view) override;
  bool BindDynamicResource(ShaderStage stage,
                           DescriptorType type,
                           const SampledImageSlot& slot,
                           std::unique_ptr<ShaderMetadata> metadata,
                           std::shared_ptr<const Texture> texture,
                           raw_ptr<const Sampler> sampler) override;

 protected:
  void OnSetLabel(std::string_view label) override;
  bool OnEncodeCommands(const Context& context) const override;

 private:
  friend struct CoverageTiledRenderPassTestPeer;
  friend struct CoverageRecorderBankTestPeer;
  friend class CoverageRecorderStorage;
  template <class T>
  friend struct CoverageRecorderAllocator;

  using Name = CoverageRecorderString<128>;

  struct BufferBinding {
    ShaderStage stage;
    DescriptorType type;
    ShaderUniformSlot slot;
    const ShaderMetadata* metadata;
    std::unique_ptr<const ShaderMetadata> owned_metadata;
    BufferView view;
    std::optional<Name> slot_name;
  };
  struct TextureBinding {
    ShaderStage stage;
    DescriptorType type;
    SampledImageSlot slot;
    const ShaderMetadata* metadata;
    std::unique_ptr<const ShaderMetadata> owned_metadata;
    std::shared_ptr<const Texture> texture;
    raw_ptr<const Sampler> sampler;
    std::optional<Name> slot_name;
  };
  using Binding = std::variant<BufferBinding, TextureBinding>;
  struct DrawPacket {
    PipelineRef pipeline;
    std::shared_ptr<Pipeline<PipelineDescriptor>> pipeline_owner;
    std::span<BufferView> vertices;
    BufferView indices;
    IndexType index_type = IndexType::kNone;
    std::span<Binding> bindings;
    std::optional<Viewport> viewport;
    std::optional<IRect32> scissor;
    std::optional<Rect> coverage;
    std::shared_ptr<const AvioSample4ClipDescriptor> sample4_clip;
    std::optional<AvioSample4SourceProof> sample4_source_proof;
    bool sample4_clip_complete = true;
    bool clip_operation = false;
    std::shared_ptr<const AvioContinuousClipExpression> continuous_clip;
    BufferView continuous_expression_buffer;
    bool continuous_geometry = false;
    std::optional<AvioContinuousPrimitive> continuous_geometry_primitive;
    Name label;
    uint32_t stencil_reference = 0;
    uint64_t base_vertex = 0;
    uint32_t element_count = 0;
    uint32_t instance_count = 1;
  };

  // A linked view into the context's aggregate bank, not a per-pass array
  // sized to the maximum scene. Packet addresses stay immutable until the
  // final strong pass owner releases their bank generation.
  struct PacketSequence {
    static constexpr uint32_t kEnd = UINT32_MAX;
    struct Iterator {
      CoverageRecorderStorage* storage;
      uint32_t index;
      const DrawPacket& operator*() const;
      Iterator& operator++();
      bool operator!=(const Iterator& other) const {
        return index != other.index;
      }
    };
    CoverageRecorderStorage* storage = nullptr;
    uint32_t head = kEnd;
    uint32_t tail = kEnd;
    size_t count = 0;
    std::array<uint16_t, 128> index_chunks = {};
    bool empty() const { return count == 0; }
    size_t size() const { return count; }
    const DrawPacket& front() const { return (*this)[0]; }
    const DrawPacket& back() const { return (*this)[count - 1]; }
    Iterator begin() const { return {storage, head}; }
    Iterator end() const { return {storage, kEnd}; }
    const DrawPacket& operator[](size_t index) const;
  };

  CoverageTiledRenderPass(const ContentContext& renderer,
                          const RenderTarget& parent,
                          RenderTarget island,
                          std::shared_ptr<AvioCoverageRegion> region,
                          std::shared_ptr<CommandBuffer> command_buffer,
                          bool tiled,
                          std::shared_ptr<CoverageRecorderStorage> storage,
                          size_t control_slot);
  static std::shared_ptr<CoverageTiledRenderPass> Claim(
      const ContentContext& renderer,
      const RenderTarget& parent,
      RenderTarget island,
      std::shared_ptr<AvioCoverageRegion> region,
      std::shared_ptr<CommandBuffer> command_buffer,
      bool tiled);
  bool AcceptMutation();
  bool TransferResourceCustody() const;
  bool EncodeTiles() const;
  bool EncodeNativeRange(IRect writable) const;
  bool RestoreNativeClip(RenderPass& pass, IRect raster) const;
  bool EncodeNative4Tile(const CoverageAtlasTile& tile,
                         const AvioSample4ClipDescriptor* clip = nullptr) const;
  std::optional<bool> EncodeCertifiedClip(IRect writable,
                                          bool validate_only = false) const;
  bool ReplayCertifiedColour(
      RenderPass& pass,
      IRect raster,
      bool translate,
      AvioCoveragePipelineVariant variant,
      const AvioSample4ClipDescriptor* clip = nullptr) const;
  bool CompositeFringeLayer(const std::shared_ptr<Texture>& source,
                            IRect local,
                            IRect destination,
                            const AvioSample4ClipDescriptor& clip) const;
  bool EncodeDirect1x() const;
  bool Replay(RenderPass& pass,
              IRect raster_rect,
              bool translate_to_raster = true) const;
  std::optional<IRect> GetPacketScissor(const DrawPacket& packet,
                                        IRect raster_rect) const;
  bool PreparePacketTextures(BlitPass& pass, IRect raster_rect) const;
  bool ReplayPacket(
      RenderPass& pass,
      const DrawPacket& packet,
      IRect raster_rect,
      bool translate_to_raster,
      std::optional<AvioCoveragePipelineVariant> variant = std::nullopt,
      const AvioSample4ClipDescriptor* clip = nullptr) const;
  bool EncodeTileReplay(std::shared_ptr<RenderPass> pass,
                        IRect raster_rect) const;
  std::optional<Rect> GetColourCoverage(IRect writable) const;
  bool CopyTile(const std::shared_ptr<Texture>& resolve,
                const std::shared_ptr<Texture>& parent,
                IRect local_content,
                IRect content) const;
  bool CompositeTile(const std::shared_ptr<Texture>& resolve,
                     const std::shared_ptr<Texture>& parent,
                     IRect local_content,
                     IRect content) const;

  const ContentContext& renderer_;
  const RenderTarget parent_;
  const RenderTarget island_;
  const std::shared_ptr<AvioCoverageRegion> region_;
  const std::shared_ptr<CommandBuffer> command_buffer_;
  const bool tiled_;
  std::shared_ptr<const AvioSample4ClipDescriptor> avio_sample4_clip_;
  bool avio_sample4_clip_complete_ = true;
  std::optional<AvioSample4SourceProof> sample4_source_proof_;
  struct EncodingRange {
    struct Iterator {
      const CoverageTiledRenderPass* owner;
      size_t index;
      const DrawPacket& operator*() const { return owner->packets_[index]; }
      Iterator& operator++() {
        ++index;
        return *this;
      }
      bool operator!=(const Iterator& other) const {
        return index != other.index;
      }
    };
    const CoverageTiledRenderPass* owner;
    size_t first;
    size_t last;
    Iterator begin() const { return {owner, first}; }
    Iterator end() const { return {owner, last}; }
  };
  EncodingRange EncodingPackets() const {
    return {this, encoding_first_, std::min(encoding_last_, packets_.size())};
  }
  mutable size_t encoding_first_ = 0;
  mutable size_t encoding_last_ = SIZE_MAX;
  mutable std::shared_ptr<const AvioSample4ClipDescriptor> native_start_clip_;
  DrawPacket pending_;
  CoverageRecorderVector<BufferView, 16> pending_vertices_;
  CoverageRecorderVector<Binding, 32> pending_bindings_;
  CoverageRecorderVector<std::shared_ptr<void>, 64> logical_owners_;
  struct ClipBuffer {
    std::shared_ptr<const AvioContinuousClipExpression> expression;
    BufferView buffer;
  };
  static constexpr size_t kMaxContinuousStates = 128;
  std::array<ClipBuffer, kMaxContinuousStates> continuous_buffers_{};
  size_t continuous_buffers_count_ = 0;
  PacketSequence packets_;
  std::shared_ptr<CoverageRecorderStorage> storage_;
  const size_t control_slot_;
  std::optional<Viewport> viewport_;
  std::optional<IRect32> scissor_;
  std::optional<Rect> coverage_;
  uint32_t stencil_reference_ = 0;
  Name label_;
  mutable bool valid_ = true;
  mutable bool encode_attempted_ = false;
  mutable bool encode_succeeded_ = false;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_ENTITY_COVERAGE_TILED_RENDER_PASS_H_
