// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Patch 60 continuous-coverage shader checks. The size budget is CPU-only.
// The evaluator fixture executes the evaluator itself on a real Vulkan
// device. Every primitive kind, own geometry, ordered intersect/difference
// expressions up to the 16-primitive cap and both final-blend paths draw
// through avio_continuous_solid_fill into native-four float32 samples. The
// resolved float bytes and their digest are exported so a shader-only change
// can be proven exact: two builds must export identical bytes on one ICD.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "flutter/fml/closure.h"
#include "flutter/fml/mapping.h"
#include "flutter/fml/synchronization/waitable_event.h"
#include "gtest/gtest.h"
#include "impeller/core/continuous_coverage.h"
#include "impeller/core/formats.h"
#include "impeller/core/sampler_descriptor.h"
#include "impeller/entity/avio_continuous_solid_fill.frag.h"
#include "impeller/entity/contents/continuous_clip.h"
#include "impeller/entity/contents/uber_sdf_parameters.h"
#include "impeller/entity/solid_fill.vert.h"
#include "impeller/entity/vk/entity_shaders_vk.h"
#include "impeller/geometry/arc.h"
#include "impeller/golden_tests/golden_renderer_vk.h"
#include "impeller/golden_tests/working_directory.h"
#include "impeller/renderer/backend/vulkan/capabilities_vk.h"
#include "impeller/renderer/blit_pass.h"
#include "impeller/renderer/command_buffer.h"
#include "impeller/renderer/command_queue.h"
#include "impeller/renderer/continuous_coverage_pipeline.h"
#include "impeller/renderer/pipeline_builder.h"
#include "impeller/renderer/pipeline_library.h"
#include "impeller/renderer/render_pass.h"
#include "impeller/renderer/render_target.h"
#include "impeller/renderer/sampler_library.h"
#include "impeller/renderer/vertex_buffer_builder.h"
#include "impeller/shader_archive/shader_archive.h"

namespace impeller::testing {
namespace {

using VS = SolidFillVertexShader;
using FS = AvioContinuousSolidFillFragmentShader;

constexpr int kTile = 48;
constexpr int kColumns = 8;
constexpr PixelFormat kFormat = PixelFormat::kR32G32B32A32Float;
constexpr Color kDestination{0.125f, 0.25f, 0.5f, 0.75f};
constexpr Color kSource{0.875f, 0.625f, 0.375f, 0.8125f};

struct ShaderCase {
  std::optional<AvioContinuousPrimitive> own;
  std::vector<std::pair<AvioContinuousPrimitive, bool>> clips;
  bool source_over = false;
  bool empty = false;
};

AvioContinuousPrimitive Physical(const AvioContinuousClip& clip,
                                 const Matrix& transform) {
  const auto primitive = clip.Transform(transform);
  FML_CHECK(primitive.has_value());
  return *primitive;
}

AvioContinuousClip GeometryClip(const UberSDFParameters& params) {
  const auto clip = AvioContinuousClip::Geometry(params);
  FML_CHECK(clip.has_value());
  return *clip;
}

// Shapes are authored in a tile's local space around (24, 24); the transforms
// add fractional phase, rotation, non-uniform scale and shear.
std::vector<ShaderCase> MakeCases() {
  const Point c{24, 24};
  const std::array<Matrix, 3> transforms = {
      Matrix::MakeTranslation({0.3125f, 0.6875f, 0}),
      Matrix::MakeTranslation({c.x + 0.37f, c.y + 0.11f, 0}) *
          Matrix::MakeRotationZ(Degrees(17)) *
          Matrix::MakeScale({1.25f, 1.25f, 1}) *
          Matrix::MakeTranslation({-c.x, -c.y, 0}),
      Matrix::MakeTranslation({c.x - 0.21f, c.y + 0.43f, 0}) *
          Matrix::MakeRotationZ(Degrees(-31)) * Matrix::MakeSkew(0.2f, 0) *
          Matrix::MakeScale({1.5f, 0.75f, 1}) *
          Matrix::MakeTranslation({-c.x, -c.y, 0}),
  };
  const auto bounds = Rect::MakeXYWH(7.25f, 9.5f, 33.5f, 29.25f);
  RoundingRadii asymmetric;
  asymmetric.top_left = {12, 5};
  asymmetric.top_right = {3, 9};
  asymmetric.bottom_left = {0, 0};
  asymmetric.bottom_right = {7, 7};
  const auto rect = AvioContinuousClip::RectClip(bounds);
  const auto rrect =
      AvioContinuousClip::RoundRectClip(RoundRect::MakeRectRadius(bounds, 9));
  const auto rrect_asymmetric = AvioContinuousClip::RoundRectClip(
      RoundRect::MakeRectRadii(bounds, asymmetric));
  const auto rse = AvioContinuousClip::SuperellipseClip(
      RoundSuperellipse::MakeRectRadius(bounds, 11));
  const auto rse_asymmetric = AvioContinuousClip::SuperellipseClip(
      RoundSuperellipse::MakeRectRadii(bounds, asymmetric));
  const auto oval = AvioContinuousClip::OvalClip(bounds);
  const auto empty = AvioContinuousClip::RectClip(Rect());
  const auto bordered = GeometryClip(UberSDFParameters::MakeBorderedRoundedRect(
      Color::White(), RoundRect::MakeRectRadius(bounds, 10),
      RoundRect::MakeRectRadius(bounds.Expand(-4.75f, -3.5f), 6)));
  const auto bordered_stroke = [&] {
    auto stroke = bordered;
    stroke.primitive.vectors[12] = {1.75f, 1, 0, 4};
    return stroke;
  }();
  const auto arc_fill = GeometryClip(UberSDFParameters::MakeArc(
      Color::White(), Arc(bounds, Degrees(12), Degrees(271), false),
      std::nullopt));
  const auto arc_round = GeometryClip(UberSDFParameters::MakeArc(
      Color::White(), Arc(bounds, Degrees(-40), Degrees(205), false),
      StrokeParameters{.width = 3.5f, .cap = Cap::kRound}));
  const auto arc_center = GeometryClip(UberSDFParameters::MakeArc(
      Color::White(), Arc(bounds, Degrees(30), Degrees(120), true),
      StrokeParameters{.width = 2.25f, .join = Join::kBevel}));
  const auto wedge = GeometryClip(UberSDFParameters::MakeArc(
      Color::White(), Arc(bounds, Degrees(200), Degrees(95), true),
      std::nullopt));

  const std::array kinds = {&rect, &rrect, &rrect_asymmetric,
                            &rse,  &oval,  &rse_asymmetric};
  std::vector<ShaderCase> cases;
  for (const auto& m : transforms) {
    for (const auto* clip : {&rect, &rrect, &rrect_asymmetric, &rse,
                             &rse_asymmetric, &oval, &bordered, &arc_fill}) {
      cases.push_back({.clips = {{Physical(*clip, m), false}}});
    }
    for (const auto* own :
         {&bordered, &bordered_stroke, &arc_round, &arc_center, &wedge}) {
      cases.push_back({.own = Physical(*own, m)});
    }
    // Own geometry combines with an ordered expression before one coverage.
    cases.push_back({.own = Physical(arc_round, m),
                     .clips = {{Physical(rrect, m), false},
                               {Physical(oval, Matrix()), true}}});
    ShaderCase deep{.source_over = true};
    for (size_t i = 0; i < kAvioContinuousMaxClips; ++i) {
      // Nested intersections narrow the shape; differences punch small
      // disjoint holes, so every slot changes some samples.
      const bool difference = i % 4 == 3;
      const float scale = difference ? 0.2f : 1.f - 0.02f * i;
      const Point shift =
          difference ? Point(-7.f + 4.5f * (i / 4), 5.f - 3.5f * (i / 4))
                     : Point();
      const auto local =
          Matrix::MakeTranslation({c.x + shift.x, c.y + shift.y, 0}) *
          Matrix::MakeScale({scale, scale, 1}) *
          Matrix::MakeTranslation({-c.x, -c.y, 0});
      deep.clips.push_back({Physical(*kinds[i % 6], m * local), difference});
    }
    cases.push_back(std::move(deep));
    cases.push_back(
        {.clips = {{Physical(rse, m), false}, {Physical(empty, m), false}},
         .source_over = true,
         .empty = true});
  }
  return cases;
}

std::shared_ptr<Texture> MakeTexture(const Context& context,
                                     ISize size,
                                     SampleCount samples) {
  TextureDescriptor desc;
  desc.storage_mode = StorageMode::kDevicePrivate;
  desc.format = kFormat;
  desc.size = size;
  desc.sample_count = samples;
  desc.type = samples == SampleCount::kCount4
                  ? TextureType::kTexture2DMultisample
                  : TextureType::kTexture2D;
  desc.usage = TextureUsage::kRenderTarget | TextureUsage::kShaderRead;
  return context.GetResourceAllocator()->CreateTexture(desc);
}

bool Submit(const std::shared_ptr<Context>& context,
            const std::shared_ptr<CommandBuffer>& commands) {
  struct Completion {
    fml::AutoResetWaitableEvent event;
    bool completed = false;
  };
  auto completion = std::make_shared<Completion>();
  if (!context->GetCommandQueue()
           ->Submit({commands},
                    [completion](CommandBuffer::Status status) {
                      completion->completed =
                          status == CommandBuffer::Status::kCompleted;
                      completion->event.Signal();
                    })
           .ok()) {
    return false;
  }
  return !completion->event.WaitWithTimeout(fml::TimeDelta::FromSeconds(60)) &&
         completion->completed;
}

BufferView Upload(const Context& context, const void* data, size_t length) {
  auto buffer = context.GetResourceAllocator()->CreateBufferWithCopy(
      static_cast<const uint8_t*>(data), length);
  FML_CHECK(buffer);
  return BufferView(std::move(buffer), Range(0, length));
}

uint64_t Fnv1a(const uint8_t* bytes, size_t length) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < length; ++i) {
    hash = (hash ^ bytes[i]) * 0x100000001b3ull;
  }
  return hash;
}

}  // namespace

// ShaderLibraryVK creates a module for every archived shader at context
// creation and the driver retains each module's SPIR-V for the context's life.
// The continuous evaluator has one inlined call site per variant; the budgets
// are the measured single-call-site sizes (largest 139,748 bytes, total
// 3,117,028 bytes) with ~5% headroom. Inlining the evaluator per use made each
// variant ~0.5 MiB and the set 20.8 MiB.
TEST(AvioContinuousShaderVK, VariantsStayWithinSpirvBudget) {
  constexpr size_t kVariants = 44u;
  constexpr size_t kMaxVariantBytes = 144u * 1024u;
  constexpr size_t kMaxTotalBytes = 3200u * 1024u;
  auto archive = ShaderArchive::Create(std::make_shared<fml::NonOwnedMapping>(
      impeller_entity_shaders_vk_data, impeller_entity_shaders_vk_length));
  ASSERT_TRUE(archive.ok());
  size_t variants = 0u;
  size_t total = 0u;
  archive->IterateAllShaders([&](ArchiveShaderType type,
                                 const std::string& name,
                                 const std::shared_ptr<fml::Mapping>& code) {
    if (name.find("avio_continuous_") == std::string::npos) {
      return true;
    }
    EXPECT_EQ(type, ArchiveShaderType::kFragment) << name;
    EXPECT_LE(code->GetSize(), kMaxVariantBytes) << name;
    variants++;
    total += code->GetSize();
    return true;
  });
  EXPECT_EQ(variants, kVariants);
  EXPECT_LE(total, kMaxTotalBytes);
  RecordProperty("ContinuousSpirvBytes", static_cast<int>(total));
}

TEST(AvioContinuousShaderVK, EvaluatorExportsExactNativeFourSamples) {
  auto context = MakeHeadlessGoldenContextVK(AvioGoldenPolicyVK::kMsaa4);
  ASSERT_TRUE(context && context->IsValid());
  const fml::ScopedCleanupClosure shutdown([&] { context->Shutdown(); });
  // gl_SampleID/gl_SamplePosition need the enabled logical-device feature.
  ASSERT_TRUE(CapabilitiesVK::Cast(*context->GetCapabilities())
                  .SupportsAvioContinuousCoverageResources());

  const auto cases = MakeCases();
  const int rows = static_cast<int>((cases.size() + kColumns - 1) / kColumns);
  const ISize size{kColumns * kTile, rows * kTile};

  auto desc = PipelineBuilder<VS, FS>::MakeDefaultPipelineDescriptor(*context);
  ASSERT_TRUE(desc.has_value());
  desc->SetSampleCount(SampleCount::kCount4);
  desc->ClearStencilAttachments();
  desc->ClearDepthAttachment();
  ColorAttachmentDescriptor colour;
  colour.format = kFormat;
  colour.blending_enabled = false;
  desc->SetColorAttachmentDescriptor(0, colour);
  auto pipeline = context->GetPipelineLibrary()->GetPipeline(desc).Get();
  ASSERT_TRUE(pipeline);

  auto destination = MakeTexture(*context, size, SampleCount::kCount4);
  auto samples = MakeTexture(*context, size, SampleCount::kCount4);
  auto resolved = MakeTexture(*context, size, SampleCount::kCount1);
  ASSERT_TRUE(destination && samples && resolved);
  auto sampler = context->GetSamplerLibrary()->GetSampler(SamplerDescriptor{});

  auto commands = context->CreateCommandBuffer();
  ASSERT_TRUE(commands);
  {
    // The destination is a distinct native-four image, as the prefix is.
    RenderTarget target;
    ColorAttachment attachment;
    attachment.texture = destination;
    attachment.load_action = LoadAction::kClear;
    attachment.store_action = StoreAction::kStore;
    attachment.clear_color = kDestination;
    target.SetColorAttachment(attachment, 0);
    auto pass = commands->CreateRenderPass(target);
    ASSERT_TRUE(pass && pass->EncodeCommands());
    // Production's prefix barrier: outside passes, before it is sampled.
    auto blit = commands->CreateBlitPass();
    ASSERT_TRUE(blit && blit->ConvertTextureToShaderRead(destination) &&
                blit->EncodeCommands());
  }
  {
    RenderTarget target;
    ColorAttachment attachment;
    attachment.texture = samples;
    attachment.resolve_texture = resolved;
    attachment.load_action = LoadAction::kClear;
    attachment.store_action = StoreAction::kMultisampleResolve;
    attachment.clear_color = Color::BlackTransparent();
    target.SetColorAttachment(attachment, 0);
    auto pass = commands->CreateRenderPass(target);
    ASSERT_TRUE(pass);
    VS::FrameInfo frame_info;
    frame_info.mvp = Matrix::MakeOrthographic(size);
    const auto frame = Upload(*context, &frame_info, sizeof(frame_info));
    FS::FragInfo frag_info;
    frag_info.color = kSource;
    const auto frag = Upload(*context, &frag_info, sizeof(frag_info));
    ColorAttachmentDescriptor source_over;
    source_over.blending_enabled = true;
    source_over.src_color_blend_factor = BlendFactor::kOne;
    source_over.src_alpha_blend_factor = BlendFactor::kOne;
    for (size_t i = 0; i < cases.size(); ++i) {
      const auto& shader_case = cases[i];
      const IPoint origin{static_cast<int64_t>(i % kColumns) * kTile,
                          static_cast<int64_t>(i / kColumns) * kTile};
      const Point corner{static_cast<Scalar>(origin.x),
                         static_cast<Scalar>(origin.y)};
      AvioContinuousClipExpression expression;
      for (const auto& [primitive, difference] : shader_case.clips) {
        expression.primitives[expression.count] = primitive;
        expression.primitives[expression.count++].vectors[0][1] =
            difference ? 1.f : 0.f;
      }
      auto control = MakeAvioContinuousControl(
          shader_case.source_over ? source_over : ColorAttachmentDescriptor{},
          expression.count, -origin,
          shader_case.own ? &*shader_case.own : nullptr);
      if (!shader_case.source_over) {
        control.operations[2] = 0;
      }
      const auto quad = Rect::MakeOriginSize(corner, Size(kTile, kTile));
      VertexBufferBuilder<VS::PerVertexData> vertices;
      vertices.AddVertices({{quad.GetLeftTop()},
                            {quad.GetRightTop()},
                            {quad.GetLeftBottom()},
                            {quad.GetRightTop()},
                            {quad.GetLeftBottom()},
                            {quad.GetRightBottom()}});
      pass->SetPipeline(pipeline);
      ASSERT_TRUE(pass->SetVertexBuffer(
          vertices.CreateVertexBuffer(*context->GetResourceAllocator())));
      ASSERT_TRUE(VS::BindFrameInfo(*pass, frame));
      ASSERT_TRUE(FS::BindFragInfo(*pass, frag));
      ASSERT_TRUE(FS::BindAvioContinuousControl(
          *pass, Upload(*context, &control, sizeof(control))));
      ASSERT_TRUE(FS::BindAvioContinuousExpression(
          *pass, Upload(*context, expression.primitives.data(),
                        sizeof(expression.primitives))));
      ASSERT_TRUE(FS::BindAvioDestination(*pass, destination, sampler));
      ASSERT_TRUE(pass->Draw().ok());
    }
    ASSERT_TRUE(pass->EncodeCommands());
  }
  const size_t bytes = static_cast<size_t>(size.Area()) * 16u;
  DeviceBufferDescriptor readback_desc;
  readback_desc.storage_mode = StorageMode::kHostVisible;
  readback_desc.readback = true;
  readback_desc.size = bytes;
  auto readback = context->GetResourceAllocator()->CreateBuffer(readback_desc);
  ASSERT_TRUE(readback);
  auto blit = commands->CreateBlitPass();
  ASSERT_TRUE(blit && blit->AddCopy(resolved, readback) &&
              blit->EncodeCommands());
  ASSERT_TRUE(Submit(context, commands));
  readback->Invalidate();
  ASSERT_TRUE(readback->OnGetContents());
  std::vector<float> pixels(bytes / sizeof(float));
  std::memcpy(pixels.data(), readback->OnGetContents(), bytes);

  // The evaluator ran: every tile has exact destination far outside its
  // shape and, unless its expression is empty, exact covered source inside.
  // Partial native-four coverage exists only where the shape has an edge.
  for (size_t i = 0; i < cases.size(); ++i) {
    SCOPED_TRACE(i);
    const int x0 = static_cast<int>(i % kColumns) * kTile;
    const int y0 = static_cast<int>(i / kColumns) * kTile;
    size_t outside = 0, inside = 0, partial = 0;
    for (int y = y0; y < y0 + kTile; ++y) {
      for (int x = x0; x < x0 + kTile; ++x) {
        const float* p = &pixels[(static_cast<size_t>(y) * size.width + x) * 4];
        if (p[0] == kDestination.red && p[3] == kDestination.alpha) {
          outside++;
        } else if (!cases[i].source_over && p[0] == kSource.red &&
                   p[3] == kSource.alpha) {
          inside++;
        } else {
          partial++;
        }
      }
    }
    EXPECT_GT(outside, 0u);
    if (cases[i].empty) {
      EXPECT_EQ(outside, static_cast<size_t>(kTile * kTile));
    } else {
      EXPECT_GT(partial, 0u);
      if (!cases[i].source_over) {
        EXPECT_GT(inside, 0u);
      }
    }
  }

  const auto digest =
      Fnv1a(reinterpret_cast<const uint8_t*>(pixels.data()), bytes);
  std::stringstream hex;
  hex << std::hex << std::setw(16) << std::setfill('0') << digest;
  RecordProperty("ResolvedFloatDigest", hex.str());
  RecordProperty("Gpu", context->DescribeGpuModel());
  std::cout << "AvioContinuousShaderVK digest " << hex.str() << " on "
            << context->DescribeGpuModel() << "\n";
  std::ofstream raw(WorkingDirectory::Instance()->GetFilenamePath(
                        "avio_continuous_shader_samples.f32"),
                    std::ios::binary | std::ios::trunc);
  raw.write(reinterpret_cast<const char*>(pixels.data()),
            static_cast<std::streamsize>(bytes));
  ASSERT_TRUE(raw.good());
}

}  // namespace impeller::testing
