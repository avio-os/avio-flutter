// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_COVERAGE_TILED_RENDER_PASS_H_
#define FLUTTER_IMPELLER_ENTITY_COVERAGE_TILED_RENDER_PASS_H_

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "impeller/renderer/render_pass.h"

namespace impeller {

class AvioCoverageRegion;
class BlitPass;
class ContentContext;

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

  struct BufferBinding {
    ShaderStage stage;
    DescriptorType type;
    ShaderUniformSlot slot;
    const ShaderMetadata* metadata;
    std::shared_ptr<const ShaderMetadata> owned_metadata;
    BufferView view;
    std::optional<std::string> slot_name;
  };
  struct TextureBinding {
    ShaderStage stage;
    DescriptorType type;
    SampledImageSlot slot;
    const ShaderMetadata* metadata;
    std::shared_ptr<const ShaderMetadata> owned_metadata;
    std::shared_ptr<const Texture> texture;
    raw_ptr<const Sampler> sampler;
    std::optional<std::string> slot_name;
  };
  using Binding = std::variant<BufferBinding, TextureBinding>;
  struct DrawPacket {
    PipelineRef pipeline;
    std::shared_ptr<Pipeline<PipelineDescriptor>> pipeline_owner;
    std::vector<BufferView> vertices;
    BufferView indices;
    IndexType index_type = IndexType::kNone;
    std::vector<Binding> bindings;
    std::optional<Viewport> viewport;
    std::optional<IRect32> scissor;
    std::optional<Rect> coverage;
    std::string label;
    uint32_t stencil_reference = 0;
    uint64_t base_vertex = 0;
    uint32_t element_count = 0;
    uint32_t instance_count = 1;
  };

  CoverageTiledRenderPass(const ContentContext& renderer,
                          const RenderTarget& parent,
                          RenderTarget island,
                          std::shared_ptr<AvioCoverageRegion> region,
                          std::shared_ptr<CommandBuffer> command_buffer,
                          bool tiled = true);
  bool AcceptMutation();
  bool EncodeTiles() const;
  bool EncodeDirect1x() const;
  bool Replay(RenderPass& pass,
              IRect raster_rect,
              bool translate_to_raster = true) const;
  std::optional<IRect> GetPacketScissor(const DrawPacket& packet,
                                        IRect raster_rect) const;
  bool PreparePacketTextures(BlitPass& pass, IRect raster_rect) const;
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
  DrawPacket pending_;
  std::vector<DrawPacket> packets_;
  std::optional<Viewport> viewport_;
  std::optional<IRect32> scissor_;
  std::optional<Rect> coverage_;
  uint32_t stencil_reference_ = 0;
  std::string label_;
  mutable bool valid_ = true;
  mutable bool encode_attempted_ = false;
  mutable bool encode_succeeded_ = false;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_ENTITY_COVERAGE_TILED_RENDER_PASS_H_
