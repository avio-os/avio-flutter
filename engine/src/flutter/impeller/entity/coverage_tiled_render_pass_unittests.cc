// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/coverage_tiled_render_pass.h"

#include <limits>

#include "gtest/gtest.h"
#include "impeller/entity/avio_coverage_region.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/renderer/testing/mocks.h"

namespace impeller {

// The friend constructs the real recorder on mock attachments without asking
// the region allocator to claim measured GPU allocations. Tests below exercise
// typed packet custody and terminal encode failure, not GPU rasterization.
struct CoverageTiledRenderPassTestPeer {
  static std::shared_ptr<CoverageTiledRenderPass> Make(
      const ContentContext& renderer,
      RenderTarget parent,
      RenderTarget island,
      std::shared_ptr<CommandBuffer> command,
      bool tiled = true,
      std::shared_ptr<AvioCoverageRegion> region = nullptr) {
    return std::shared_ptr<CoverageTiledRenderPass>(new CoverageTiledRenderPass(
        renderer, parent, std::move(island), std::move(region),
        std::move(command), tiled));
  }
  static const auto& Packets(const CoverageTiledRenderPass& pass) {
    return pass.packets_;
  }
  static const auto& Buffer(const CoverageTiledRenderPass& pass,
                            size_t packet,
                            size_t binding) {
    return std::get<CoverageTiledRenderPass::BufferBinding>(
        pass.packets_[packet].bindings[binding]);
  }
  static const auto& Texture(const CoverageTiledRenderPass& pass,
                             size_t packet,
                             size_t binding) {
    return std::get<CoverageTiledRenderPass::TextureBinding>(
        pass.packets_[packet].bindings[binding]);
  }
  static bool Replay(const CoverageTiledRenderPass& pass,
                     RenderPass& target,
                     IRect raster,
                     bool translate_to_raster = true) {
    return pass.Replay(target, raster, translate_to_raster);
  }
  static std::optional<Rect> ColourCoverage(const CoverageTiledRenderPass& pass,
                                            IRect writable) {
    return pass.GetColourCoverage(writable);
  }
  static bool Prepare(const CoverageTiledRenderPass& pass,
                      BlitPass& blit,
                      IRect raster) {
    return pass.PreparePacketTextures(blit, raster);
  }
  static bool Composite(const CoverageTiledRenderPass& pass,
                        IRect local_content,
                        IRect content) {
    return pass.CompositeTile(
        pass.island_.GetColorAttachment(0).resolve_texture,
        pass.parent_.GetColorAttachment(0).texture, local_content, content);
  }
};

namespace testing {
namespace {

class RecorderPipeline final : public Pipeline<PipelineDescriptor> {
 public:
  explicit RecorderPipeline(bool depth_only = false,
                            SampleCount samples = SampleCount::kCount4)
      : Pipeline({}, Descriptor(depth_only, samples)) {}
  bool IsValid() const override { return true; }

 private:
  static PipelineDescriptor Descriptor(bool depth_only, SampleCount samples) {
    PipelineDescriptor result;
    ColorAttachmentDescriptor colour;
    colour.format = PixelFormat::kB8G8R8A8UNormInt;
    colour.write_mask =
        depth_only ? ColorWriteMaskBits::kNone : ColorWriteMaskBits::kAll;
    result.SetSampleCount(samples);
    result.SetColorAttachmentDescriptor(0, colour);
    return result;
  }
};

class RecorderPreparationBlit : public MockBlitPass {
 public:
  MOCK_METHOD(bool,
              ConvertTextureToShaderRead,
              (const std::shared_ptr<Texture>& texture),
              (override));
};

// The CPU allocator fixture reports an explicit backing size to the real
// region factory. It does not assert any Vulkan allocation/raster result.
class RecorderAllocatedTexture : public MockTexture {
 public:
  explicit RecorderAllocatedTexture(const TextureDescriptor& desc)
      : MockTexture(desc) {}
  size_t GetAllocatedByteSize() const override {
    const auto& desc = GetTextureDescriptor();
    if (desc.format == PixelFormat::kD32FloatS8UInt) {
      return desc.size.width * desc.size.height * 8u *
             static_cast<size_t>(desc.sample_count);
    }
    return desc.GetByteSizeOfAllMipLevels() *
           static_cast<size_t>(desc.sample_count);
  }
};

class CoverageTiledRenderPassTest : public ::testing::Test {
 protected:
  void SetUp() override {
    using ::testing::_;
    using ::testing::Return;
    using ::testing::ReturnRef;
    context = std::make_shared<::testing::NiceMock<MockImpellerContext>>();
    allocator = std::make_shared<::testing::NiceMock<MockAllocator>>();
    auto capabilities =
        std::make_shared<::testing::NiceMock<MockCapabilities>>();
    capabilities_ = capabilities;
    ON_CALL(*context, GetCapabilities())
        .WillByDefault(ReturnRef(capabilities_));
    ON_CALL(*context, GetResourceAllocator()).WillByDefault(Return(allocator));
    ON_CALL(*context, IsValid()).WillByDefault(Return(false));
    ON_CALL(*allocator, OnCreateBuffer(_)).WillByDefault([](const auto& desc) {
      return std::make_shared<::testing::NiceMock<MockDeviceBuffer>>(desc);
    });
    renderer = std::make_unique<ContentContext>(context, nullptr);
    command = std::make_shared<::testing::NiceMock<MockCommandBuffer>>(context);
    ON_CALL(*command, IsValid()).WillByDefault(Return(true));
    pipeline = std::make_shared<RecorderPipeline>();
    parent = MakeTarget({600, 400}, SampleCount::kCount1);
    island = MakeTarget({256, 256}, SampleCount::kCount4);
    pass = CoverageTiledRenderPassTestPeer::Make(*renderer, parent, island,
                                                 command);
  }

  RenderTarget MakeTarget(ISize size, SampleCount samples) {
    TextureDescriptor desc;
    desc.size = size;
    desc.format = PixelFormat::kB8G8R8A8UNormInt;
    desc.sample_count = samples;
    desc.type = samples == SampleCount::kCount1
                    ? TextureType::kTexture2D
                    : TextureType::kTexture2DMultisample;
    auto texture = MakeTexture(desc);
    ColorAttachment colour;
    colour.texture = texture;
    colour.load_action = LoadAction::kLoad;
    colour.store_action = StoreAction::kStore;
    RenderTarget target;
    if (samples != SampleCount::kCount1) {
      desc.sample_count = SampleCount::kCount1;
      desc.type = TextureType::kTexture2D;
      colour.resolve_texture = MakeTexture(desc);
      desc.sample_count = samples;
      desc.type = TextureType::kTexture2DMultisample;
      desc.format = PixelFormat::kD32FloatS8UInt;
      auto depth_stencil = MakeTexture(desc);
      DepthAttachment depth;
      depth.texture = depth_stencil;
      target.SetDepthAttachment(depth);
      StencilAttachment stencil;
      stencil.texture = depth_stencil;
      target.SetStencilAttachment(stencil);
    }
    target.SetColorAttachment(colour, 0);
    return target;
  }

  std::shared_ptr<Texture> MakeTexture(TextureDescriptor desc) {
    using ::testing::Return;
    auto texture = std::make_shared<::testing::NiceMock<MockTexture>>(desc);
    ON_CALL(*texture, GetSize()).WillByDefault(Return(desc.size));
    ON_CALL(*texture, IsValid()).WillByDefault(Return(true));
    return texture;
  }

  void RecordDraw() {
    pass->SetPipeline(pipeline);
    pass->SetElementCount(3);
    ASSERT_TRUE(pass->Draw().ok());
  }

  std::shared_ptr<AvioCoverageRegion> MakeRegion() {
    using ::testing::_;
    using ::testing::Return;
    auto region_allocator =
        std::make_shared<::testing::NiceMock<MockAllocator>>();
    ON_CALL(*region_allocator, GetMaxTextureSizeSupported())
        .WillByDefault(Return(ISize(4096, 4096)));
    ON_CALL(*region_allocator, OnCreateTexture(_, _))
        .WillByDefault([](const TextureDescriptor& desc, bool) {
          auto texture =
              std::make_shared<::testing::NiceMock<RecorderAllocatedTexture>>(
                  desc);
          ON_CALL(*texture, GetSize()).WillByDefault(Return(desc.size));
          ON_CALL(*texture, IsValid()).WillByDefault(Return(true));
          return texture;
        });
    return AvioCoverageRegion::Create(region_allocator, {});
  }

  std::shared_ptr<::testing::NiceMock<MockImpellerContext>> context;
  std::shared_ptr<::testing::NiceMock<MockAllocator>> allocator;
  std::shared_ptr<const Capabilities> capabilities_;
  std::unique_ptr<ContentContext> renderer;
  std::shared_ptr<::testing::NiceMock<MockCommandBuffer>> command;
  std::shared_ptr<RecorderPipeline> pipeline;
  RenderTarget parent;
  RenderTarget island;
  std::shared_ptr<CoverageTiledRenderPass> pass;
};

TEST_F(CoverageTiledRenderPassTest, LogicalCoordinatesKeepPhysicalAttachments) {
  EXPECT_EQ(pass->GetRenderTargetSize(), ISize(600, 400));
  EXPECT_EQ(pass->GetRenderTarget().GetRenderTargetSize(), ISize(256, 256));
  EXPECT_EQ(pass->GetSampleCount(), SampleCount::kCount4);
  EXPECT_TRUE(pass->HasDepthAttachment());
  EXPECT_TRUE(pass->HasStencilAttachment());
  EXPECT_EQ(pass->GetOrthographicTransform(),
            Matrix::MakeOrthographic(ISize(600, 400)));
}

TEST_F(CoverageTiledRenderPassTest, PersistentClipStateSurvivesDrawBoundaries) {
  Viewport viewport{.rect = Rect::MakeXYWH(12, 20, 500, 300)};
  auto scissor = IRect32::MakeXYWH(15, 23, 200, 150);
  pass->SetViewport(viewport);
  pass->SetScissor(scissor);
  pass->SetStencilReference(7);
  pass->SetBaseVertex(17);
  RecordDraw();
  RecordDraw();
  const auto& packets = CoverageTiledRenderPassTestPeer::Packets(*pass);
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_EQ(packets[0].viewport->rect, viewport.rect);
  EXPECT_EQ(packets[1].viewport->rect, viewport.rect);
  EXPECT_EQ(packets[0].scissor, scissor);
  EXPECT_EQ(packets[1].scissor, scissor);
  EXPECT_EQ(packets[0].stencil_reference, 7u);
  EXPECT_EQ(packets[1].stencil_reference, 7u);
  EXPECT_EQ(packets[0].base_vertex, 17u);
  EXPECT_EQ(packets[1].base_vertex, 0u);
}

TEST_F(CoverageTiledRenderPassTest, DynamicBindingsOwnMetadataNamesAndBuffer) {
  DeviceBufferDescriptor desc;
  desc.size = 64;
  auto buffer = std::make_shared<::testing::NiceMock<MockDeviceBuffer>>(desc);
  std::weak_ptr<const DeviceBuffer> weak_buffer = buffer;
  std::string caller_name = "runtime_binding";
  ShaderUniformSlot slot{caller_name.c_str(), 2, 0, 11};
  auto metadata = std::make_unique<ShaderMetadata>();
  ASSERT_TRUE(pass->BindDynamicResource(
      ShaderStage::kFragment, DescriptorType::kUniformBuffer, slot,
      std::move(metadata), BufferView(buffer, Range{8, 16})));
  caller_name.assign("changed");
  buffer.reset();
  RecordDraw();
  RecordDraw();
  const auto& binding = CoverageTiledRenderPassTestPeer::Buffer(*pass, 0, 0);
  EXPECT_EQ(binding.stage, ShaderStage::kFragment);
  EXPECT_EQ(binding.type, DescriptorType::kUniformBuffer);
  EXPECT_EQ(binding.slot.binding, 11u);
  EXPECT_EQ(binding.slot.ext_res_0, 2u);
  EXPECT_EQ(binding.slot_name, "runtime_binding");
  EXPECT_EQ(binding.metadata, binding.owned_metadata.get());
  EXPECT_EQ(binding.view.GetRange().offset, 8u);
  EXPECT_EQ(binding.view.GetRange().length, 16u);
  EXPECT_FALSE(weak_buffer.expired());
  const auto& packets = CoverageTiledRenderPassTestPeer::Packets(*pass);
  EXPECT_TRUE(packets[1].bindings.empty());
  pass.reset();
  EXPECT_TRUE(weak_buffer.expired());
}

TEST_F(CoverageTiledRenderPassTest, IndexAndVertexViewsStayOwnedUntilEncoding) {
  DeviceBufferDescriptor desc;
  desc.size = 64;
  auto buffer = std::make_shared<::testing::NiceMock<MockDeviceBuffer>>(desc);
  std::weak_ptr<const DeviceBuffer> weak_buffer = buffer;
  BufferView views[] = {BufferView(buffer, Range{0, 16})};
  ASSERT_TRUE(pass->SetVertexBuffer(views, 1));
  ASSERT_TRUE(pass->SetIndexBuffer(BufferView(buffer, Range{16, 6}),
                                   IndexType::k16bit));
  views[0] = BufferView{};
  buffer.reset();
  RecordDraw();
  EXPECT_FALSE(weak_buffer.expired());
  const auto& packet = CoverageTiledRenderPassTestPeer::Packets(*pass)[0];
  ASSERT_EQ(packet.vertices.size(), 1u);
  EXPECT_EQ(packet.index_type, IndexType::k16bit);
  EXPECT_EQ(packet.indices.GetRange().offset, 16u);
  pass.reset();
  EXPECT_TRUE(weak_buffer.expired());
}

TEST_F(CoverageTiledRenderPassTest, BindingFailurePoisonsTheRecordedSegment) {
  ShaderUniformSlot slot{"missing", 0, 0, 0};
  EXPECT_FALSE(pass->BindResource(ShaderStage::kVertex,
                                  DescriptorType::kUniformBuffer, slot, nullptr,
                                  BufferView{}));
  pass->SetPipeline(pipeline);
  pass->SetElementCount(3);
  EXPECT_FALSE(pass->Draw().ok());
  EXPECT_FALSE(pass->IsValid());
  EXPECT_TRUE(CoverageTiledRenderPassTestPeer::Packets(*pass).empty());
}

TEST_F(CoverageTiledRenderPassTest, CountOverflowFailsInsteadOfTruncating) {
  pass->SetElementCount(
      static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1);
  pass->SetPipeline(pipeline);
  EXPECT_FALSE(pass->Draw().ok());
  EXPECT_FALSE(pass->IsValid());
}

TEST_F(CoverageTiledRenderPassTest, FailedEncodeIsConsumedExactlyOnce) {
  using ::testing::_;
  using ::testing::Return;
  EXPECT_CALL(*command, OnCreateRenderPass(_))
      .Times(1)
      .WillOnce(Return(nullptr));
  EXPECT_FALSE(pass->EncodeCommands());
  EXPECT_FALSE(pass->EncodeCommands());
  EXPECT_FALSE(pass->IsValid());
}

TEST_F(CoverageTiledRenderPassTest, MissingWarmRegionCannotAllocateAFallback) {
  EXPECT_EQ(CoverageTiledRenderPass::Make(*renderer, parent, command), nullptr);
}

TEST_F(CoverageTiledRenderPassTest,
       LogicalLayerViewKeepsPhysicalTextureHonest) {
  ASSERT_TRUE(parent.SetContentRect(IRect::MakeSize(ISize(127, 89))));
  pass =
      CoverageTiledRenderPassTestPeer::Make(*renderer, parent, island, command);
  EXPECT_EQ(pass->GetRenderTargetSize(), ISize(127, 89));
  EXPECT_EQ(parent.GetRenderTargetTexture()->GetSize(), ISize(600, 400));
  EXPECT_EQ(pass->GetOrthographicTransform(),
            Matrix::MakeOrthographic(ISize(127, 89)));
}

TEST_F(CoverageTiledRenderPassTest, AtlasCustodySurvivesRecordingCallerDrop) {
  auto owner = std::make_shared<int>(17);
  std::weak_ptr<int> weak = owner;
  pass->RetainResource(owner);
  owner.reset();
  RecordDraw();
  EXPECT_FALSE(weak.expired());
  pass.reset();
  EXPECT_FALSE(weak.expired());
  command.reset();
  EXPECT_TRUE(weak.expired());
}

TEST_F(CoverageTiledRenderPassTest,
       ParentCustodySurvivesRecorderBeforeEnqueue) {
  auto owner = std::make_shared<int>(23);
  std::weak_ptr<int> weak = owner;
  parent.SetResourceOwner(owner);
  pass = CoverageTiledRenderPassTestPeer::Make(*renderer, parent, island,
                                               command, false);
  parent.SetResourceOwner(nullptr);
  owner.reset();
  pass.reset();
  EXPECT_FALSE(weak.expired());
  command.reset();
  EXPECT_TRUE(weak.expired());
}

TEST_F(CoverageTiledRenderPassTest,
       PipelineCacheInvalidationKeepsRecordedDraw) {
  std::weak_ptr<RecorderPipeline> weak = pipeline;
  RecordDraw();
  pipeline.reset();
  EXPECT_FALSE(weak.expired());
  const auto& packet = CoverageTiledRenderPassTestPeer::Packets(*pass)[0];
  EXPECT_TRUE(packet.pipeline->IsValid());
  EXPECT_EQ(packet.pipeline_owner, weak.lock());
  pass.reset();
  EXPECT_TRUE(weak.expired());
}

TEST_F(CoverageTiledRenderPassTest, ExpiredPipelineFailsBeforeDereferencing) {
  PipelineRef reference(pipeline);
  pipeline.reset();
  pass->SetPipeline(reference);
  EXPECT_FALSE(pass->IsValid());
  EXPECT_FALSE(pass->Draw().ok());
}

TEST_F(CoverageTiledRenderPassTest, SeparateVertexCallsAppendExactStreams) {
  DeviceBufferDescriptor desc;
  desc.size = 64;
  auto buffer = std::make_shared<::testing::NiceMock<MockDeviceBuffer>>(desc);
  ASSERT_TRUE(pass->SetVertexBuffer(BufferView(buffer, Range{0, 16})));
  ASSERT_TRUE(pass->SetVertexBuffer(nullptr, 0));
  ASSERT_TRUE(pass->SetVertexBuffer(BufferView(buffer, Range{16, 16})));
  RecordDraw();
  const auto& packet = CoverageTiledRenderPassTestPeer::Packets(*pass)[0];
  ASSERT_EQ(packet.vertices.size(), 2u);
  EXPECT_EQ(packet.vertices[0].GetRange().offset, 0u);
  EXPECT_EQ(packet.vertices[1].GetRange().offset, 16u);
}

TEST_F(CoverageTiledRenderPassTest, TextureBindingsRetainExactDynamicPayload) {
  TextureDescriptor desc;
  desc.size = ISize(16, 16);
  desc.format = PixelFormat::kB8G8R8A8UNormInt;
  auto texture = MakeTexture(desc);
  std::weak_ptr<Texture> witness = texture;
  std::shared_ptr<const Sampler> sampler =
      std::make_shared<MockSampler>(SamplerDescriptor{});
  std::string name = "runtime_sampler";
  SampledImageSlot slot{name.c_str(), 3, 2, 9};
  auto metadata = std::make_unique<ShaderMetadata>();
  metadata->name = "dynamic_owned_metadata";
  ASSERT_TRUE(pass->BindDynamicResource(
      ShaderStage::kFragment, DescriptorType::kSampledImage, slot,
      std::move(metadata), texture, raw_ptr<const Sampler>(sampler)));
  texture.reset();
  name.assign("overwritten");
  RecordDraw();
  const auto& binding = CoverageTiledRenderPassTestPeer::Texture(*pass, 0, 0);
  EXPECT_EQ(binding.slot_name, "runtime_sampler");
  EXPECT_EQ(binding.slot.texture_index, 3u);
  EXPECT_EQ(binding.slot.set, 2u);
  EXPECT_EQ(binding.slot.binding, 9u);
  EXPECT_EQ(binding.metadata->name, "dynamic_owned_metadata");
  EXPECT_EQ(binding.metadata, binding.owned_metadata.get());
  EXPECT_FALSE(witness.expired());
  pass.reset();
  EXPECT_TRUE(witness.expired());
}

TEST_F(CoverageTiledRenderPassTest, OrdinaryDeferredPassRetainsAtlasCustody) {
  auto immediate =
      std::make_shared<::testing::NiceMock<MockRenderPass>>(context, island);
  auto owner = std::make_shared<int>(17);
  std::weak_ptr<int> weak = owner;
  immediate->RetainResource(owner);
  owner.reset();
  EXPECT_FALSE(weak.expired());
  immediate.reset();
  EXPECT_TRUE(weak.expired());
}

TEST_F(CoverageTiledRenderPassTest, SmallColourDrawsCullWithoutCullingClips) {
  pass->SetDrawCoverage(Rect::MakeXYWH(10, 12, 20, 30));
  RecordDraw();
  pass->SetDrawCoverage(Rect::MakeXYWH(520, 20, 30, 30));
  RecordDraw();
  auto clip_pipeline = std::make_shared<RecorderPipeline>(true);
  pass->SetPipeline(clip_pipeline);
  pass->SetDrawCoverage(Rect::MakeXYWH(540, 20, 10, 10));
  pass->SetElementCount(6);
  ASSERT_TRUE(pass->Draw().ok());
  auto replay =
      std::make_shared<::testing::NiceMock<MockRenderPass>>(context, island);
  ASSERT_TRUE(CoverageTiledRenderPassTestPeer::Replay(
      *pass, *replay, IRect::MakeXYWH(0, 0, 256, 256)));
  const auto& commands = replay->GetCommands();
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0].element_count, 3u);
  EXPECT_EQ(commands[1].element_count, 6u);
  EXPECT_EQ(commands[1].pipeline, PipelineRef(clip_pipeline));
}

TEST_F(CoverageTiledRenderPassTest, ReplayUsesLogicalViewportAndLocalScissor) {
  pass->SetDrawCoverage(Rect::MakeXYWH(280, 22, 20, 20));
  pass->SetScissor(IRect32::MakeXYWH(270, 20, 40, 30));
  RecordDraw();
  auto replay =
      std::make_shared<::testing::NiceMock<MockRenderPass>>(context, island);
  ASSERT_TRUE(CoverageTiledRenderPassTestPeer::Replay(
      *pass, *replay, IRect::MakeXYWH(256, 0, 256, 256)));
  const auto& commands = replay->GetCommands();
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].viewport->rect, Rect::MakeXYWH(-256, 0, 600, 400));
  EXPECT_EQ(commands[0].scissor, IRect32::MakeXYWH(14, 20, 40, 30));
}

TEST_F(CoverageTiledRenderPassTest, ClipBoundsDoNotExpandColourTilePlan) {
  pass->SetDrawCoverage(Rect::MakeXYWH(10, 12, 20, 30));
  RecordDraw();
  auto clip_pipeline = std::make_shared<RecorderPipeline>(true);
  pass->SetPipeline(clip_pipeline);
  pass->SetDrawCoverage(std::nullopt);
  pass->SetElementCount(6);
  ASSERT_TRUE(pass->Draw().ok());
  EXPECT_EQ(CoverageTiledRenderPassTestPeer::ColourCoverage(
                *pass, IRect::MakeSize(ISize(600, 400))),
            Rect::MakeLTRB(9, 11, 31, 43));
}

TEST_F(CoverageTiledRenderPassTest, UnknownOrNonfiniteCoverageIsConservative) {
  pass->SetDrawCoverage(
      Rect::MakeLTRB(0, 0, std::numeric_limits<Scalar>::infinity(), 100));
  RecordDraw();
  EXPECT_EQ(CoverageTiledRenderPassTestPeer::ColourCoverage(
                *pass, IRect::MakeSize(ISize(600, 400))),
            Rect::MakeSize(ISize(600, 400)));
}

TEST_F(CoverageTiledRenderPassTest, WarmLayerClearInitializesPhysicalPadding) {
  using ::testing::_;
  ASSERT_TRUE(parent.SetContentRect(IRect::MakeSize(ISize(127, 89))));
  ASSERT_TRUE(parent.SetRenderArea(IRect::MakeSize(ISize(127, 89))));
  auto colour = parent.GetColorAttachment(0);
  colour.load_action = LoadAction::kClear;
  parent.SetColorAttachment(colour, 0);
  pass =
      CoverageTiledRenderPassTestPeer::Make(*renderer, parent, island, command);
  EXPECT_CALL(*command, OnCreateRenderPass(_))
      .WillOnce([](const RenderTarget& target) -> std::shared_ptr<RenderPass> {
        EXPECT_EQ(target.GetRenderTargetSize(), ISize(600, 400));
        EXPECT_FALSE(target.GetRenderArea());
        EXPECT_EQ(target.GetColorAttachment(0).load_action, LoadAction::kClear);
        return nullptr;
      });
  EXPECT_FALSE(pass->EncodeCommands());
}

TEST_F(CoverageTiledRenderPassTest, RelevantSampledInputsPreparedBeforeReplay) {
  using ::testing::Return;
  TextureDescriptor desc;
  desc.size = ISize(16, 16);
  desc.format = PixelFormat::kB8G8R8A8UNormInt;
  auto near_texture = MakeTexture(desc);
  auto far_texture = MakeTexture(desc);
  auto clip_texture = MakeTexture(desc);
  std::shared_ptr<const Sampler> sampler =
      std::make_shared<MockSampler>(SamplerDescriptor{});
  auto bind = [&](const std::shared_ptr<Texture>& texture) {
    return pass->BindResource(ShaderStage::kFragment,
                              DescriptorType::kSampledImage,
                              SampledImageSlot{"image", 0, 0, 0}, nullptr,
                              texture, raw_ptr<const Sampler>(sampler));
  };
  pass->SetDrawCoverage(Rect::MakeXYWH(10, 12, 20, 30));
  ASSERT_TRUE(bind(near_texture));
  RecordDraw();
  pass->SetDrawCoverage(Rect::MakeXYWH(520, 20, 30, 30));
  ASSERT_TRUE(bind(far_texture));
  RecordDraw();
  auto clip_pipeline = std::make_shared<RecorderPipeline>(true);
  pass->SetPipeline(clip_pipeline);
  pass->SetDrawCoverage(Rect::MakeXYWH(540, 20, 10, 10));
  ASSERT_TRUE(bind(clip_texture));
  pass->SetElementCount(6);
  ASSERT_TRUE(pass->Draw().ok());
  ::testing::StrictMock<RecorderPreparationBlit> blit;
  EXPECT_CALL(blit, ConvertTextureToShaderRead(near_texture))
      .WillOnce(Return(true));
  EXPECT_CALL(blit, ConvertTextureToShaderRead(clip_texture))
      .WillOnce(Return(true));
  EXPECT_TRUE(CoverageTiledRenderPassTestPeer::Prepare(
      *pass, blit, IRect::MakeXYWH(0, 0, 256, 256)));
}

TEST_F(CoverageTiledRenderPassTest, FailedTexturePreparationStopsReplay) {
  using ::testing::_;
  using ::testing::Return;
  TextureDescriptor desc;
  desc.size = ISize(16, 16);
  desc.format = PixelFormat::kB8G8R8A8UNormInt;
  auto texture = MakeTexture(desc);
  std::shared_ptr<const Sampler> sampler =
      std::make_shared<MockSampler>(SamplerDescriptor{});
  ASSERT_TRUE(pass->BindResource(ShaderStage::kFragment,
                                 DescriptorType::kSampledImage,
                                 SampledImageSlot{"image", 0, 0, 0}, nullptr,
                                 texture, raw_ptr<const Sampler>(sampler)));
  RecordDraw();
  ::testing::StrictMock<RecorderPreparationBlit> blit;
  EXPECT_CALL(blit, ConvertTextureToShaderRead(_)).WillOnce(Return(false));
  EXPECT_FALSE(CoverageTiledRenderPassTestPeer::Prepare(
      *pass, blit, IRect::MakeXYWH(0, 0, 256, 256)));
}

TEST_F(CoverageTiledRenderPassTest, DirectFilterExposesLogical1xWithoutDepth) {
  ASSERT_TRUE(parent.SetContentRect(IRect::MakeSize(ISize(127, 89))));
  pass = CoverageTiledRenderPassTestPeer::Make(*renderer, parent, island,
                                               command, false);
  EXPECT_EQ(pass->GetRenderTargetSize(), ISize(127, 89));
  EXPECT_EQ(pass->GetRenderTarget().GetRenderTargetSize(), ISize(600, 400));
  EXPECT_EQ(pass->GetSampleCount(), SampleCount::kCount1);
  EXPECT_FALSE(pass->HasDepthAttachment());
  EXPECT_FALSE(pass->HasStencilAttachment());
  EXPECT_EQ(pass->GetOrthographicTransform(),
            Matrix::MakeOrthographic(ISize(127, 89)));
}

TEST_F(CoverageTiledRenderPassTest, DirectDamageScissorKeepsParentCoordinates) {
  pass = CoverageTiledRenderPassTestPeer::Make(*renderer, parent, island,
                                               command, false);
  pipeline = std::make_shared<RecorderPipeline>(false, SampleCount::kCount1);
  pass->SetDrawCoverage(Rect::MakeXYWH(280, 22, 20, 20));
  RecordDraw();
  auto replay =
      std::make_shared<::testing::NiceMock<MockRenderPass>>(context, parent);
  ASSERT_TRUE(CoverageTiledRenderPassTestPeer::Replay(
      *pass, *replay, IRect::MakeXYWH(270, 20, 40, 30), false));
  const auto& commands = replay->GetCommands();
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].viewport->rect, Rect::MakeSize(ISize(600, 400)));
  EXPECT_EQ(commands[0].scissor, IRect32::MakeXYWH(270, 20, 40, 30));
}

TEST_F(CoverageTiledRenderPassTest, DirectWarmPaddingClearDoesNotSeedAnIsland) {
  using ::testing::_;
  ASSERT_TRUE(parent.SetContentRect(IRect::MakeSize(ISize(127, 89))));
  ASSERT_TRUE(parent.SetRenderArea(IRect::MakeSize(ISize(127, 89))));
  auto colour = parent.GetColorAttachment(0);
  colour.load_action = LoadAction::kClear;
  parent.SetColorAttachment(colour, 0);
  pass = CoverageTiledRenderPassTestPeer::Make(*renderer, parent, island,
                                               command, false);
  EXPECT_CALL(*command, OnCreateBlitPass()).Times(0);
  EXPECT_CALL(*command, OnCreateRenderPass(_))
      .Times(1)
      .WillOnce([](const RenderTarget& target) -> std::shared_ptr<RenderPass> {
        EXPECT_EQ(target.GetSampleCount(), SampleCount::kCount1);
        EXPECT_FALSE(target.GetDepthAttachment());
        EXPECT_FALSE(target.GetStencilAttachment());
        EXPECT_EQ(target.GetRenderTargetSize(), ISize(600, 400));
        EXPECT_FALSE(target.GetRenderArea());
        EXPECT_EQ(target.GetColorAttachment(0).load_action, LoadAction::kClear);
        return nullptr;
      });
  EXPECT_FALSE(pass->EncodeCommands());
}

TEST_F(CoverageTiledRenderPassTest, SuccessfulCompositesCountExactCopiedTiles) {
  using ::testing::_;
  using ::testing::Return;
  auto region = MakeRegion();
  ASSERT_TRUE(region);
  pass = CoverageTiledRenderPassTestPeer::Make(*renderer, parent, island,
                                               command, true, region);
  auto blit = std::make_shared<::testing::NiceMock<MockBlitPass>>();
  ON_CALL(*blit, IsValid()).WillByDefault(Return(true));
  EXPECT_CALL(*command, OnCreateBlitPass())
      .Times(3)
      .WillRepeatedly(Return(blit));
  EXPECT_CALL(*blit, OnCopyTextureToTextureCommand(_, _, _, _, _))
      .Times(3)
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*blit, EncodeCommands()).Times(3).WillRepeatedly(Return(true));
  for (int64_t i = 0; i < 3; i++) {
    EXPECT_TRUE(CoverageTiledRenderPassTestPeer::Composite(
        *pass, IRect::MakeXYWH(0, 0, 48, 32),
        IRect::MakeXYWH(i * 48, 0, 48, 32)));
    EXPECT_EQ(region->ReportUsage(false).coverage_flushes,
              static_cast<uint64_t>(i + 1));
  }
}

TEST_F(CoverageTiledRenderPassTest, RejectedCopyCannotCountAComposite) {
  using ::testing::_;
  using ::testing::Return;
  auto region = MakeRegion();
  ASSERT_TRUE(region);
  pass = CoverageTiledRenderPassTestPeer::Make(*renderer, parent, island,
                                               command, true, region);
  auto blit = std::make_shared<::testing::NiceMock<MockBlitPass>>();
  ON_CALL(*blit, IsValid()).WillByDefault(Return(true));
  EXPECT_CALL(*command, OnCreateBlitPass()).WillOnce(Return(blit));
  EXPECT_CALL(*blit, OnCopyTextureToTextureCommand(_, _, _, _, _))
      .WillOnce(Return(false));
  EXPECT_CALL(*blit, EncodeCommands()).Times(0);
  EXPECT_FALSE(CoverageTiledRenderPassTestPeer::Composite(
      *pass, IRect::MakeXYWH(0, 0, 48, 32), IRect::MakeXYWH(0, 0, 48, 32)));
  EXPECT_EQ(region->ReportUsage(false).coverage_flushes, 0u);
}

TEST_F(CoverageTiledRenderPassTest, FailedCopyEncodingCannotCountAComposite) {
  using ::testing::_;
  using ::testing::Return;
  auto region = MakeRegion();
  ASSERT_TRUE(region);
  pass = CoverageTiledRenderPassTestPeer::Make(*renderer, parent, island,
                                               command, true, region);
  auto blit = std::make_shared<::testing::NiceMock<MockBlitPass>>();
  ON_CALL(*blit, IsValid()).WillByDefault(Return(true));
  EXPECT_CALL(*command, OnCreateBlitPass()).WillOnce(Return(blit));
  EXPECT_CALL(*blit, OnCopyTextureToTextureCommand(_, _, _, _, _))
      .WillOnce(Return(true));
  EXPECT_CALL(*blit, EncodeCommands()).WillOnce(Return(false));
  EXPECT_FALSE(CoverageTiledRenderPassTestPeer::Composite(
      *pass, IRect::MakeXYWH(0, 0, 48, 32), IRect::MakeXYWH(0, 0, 48, 32)));
  EXPECT_EQ(region->ReportUsage(false).coverage_flushes, 0u);
}

TEST_F(CoverageTiledRenderPassTest,
       Direct1xDoesNotCountColourIslandComposites) {
  using ::testing::_;
  using ::testing::Return;
  auto region = MakeRegion();
  ASSERT_TRUE(region);
  pass = CoverageTiledRenderPassTestPeer::Make(*renderer, parent, island,
                                               command, false, region);
  auto actual =
      std::make_shared<::testing::NiceMock<MockRenderPass>>(context, parent);
  ON_CALL(*actual, IsValid()).WillByDefault(Return(true));
  EXPECT_CALL(*actual, OnEncodeCommands(_)).WillOnce(Return(true));
  EXPECT_CALL(*command, OnCreateRenderPass(_)).WillOnce(Return(actual));
  EXPECT_CALL(*command, OnCreateBlitPass()).Times(0);
  EXPECT_TRUE(pass->EncodeCommands());
  EXPECT_EQ(region->ReportUsage(false).coverage_flushes, 0u);
}

}  // namespace
}  // namespace testing
}  // namespace impeller
