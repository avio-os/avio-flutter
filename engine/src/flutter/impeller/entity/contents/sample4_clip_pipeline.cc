// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "impeller/entity/contents/sample4_clip_pipeline.h"
#include "impeller/renderer/context.h"
#include "impeller/renderer/continuous_coverage_pipeline.h"
#include "impeller/renderer/pipeline_library.h"
#include "impeller/renderer/shader_function.h"
#include "impeller/renderer/shader_library.h"
#include "impeller/renderer/vertex_descriptor.h"
namespace impeller {
std::shared_ptr<Pipeline<PipelineDescriptor>> CreateAvioCoveragePipelineVariant(
    const Context& context,
    const PipelineDescriptor& original,
    AvioCoveragePipelineVariant kind) {
  if (kind == AvioCoveragePipelineVariant::kContinuous) {
    return GetAvioContinuousPipeline(context, original);
  }
  const auto* colour = original.GetColorAttachmentDescriptor(0);
  if (!colour || colour->write_mask == ColorWriteMaskBits::kNone) {
    return nullptr;
  }
  auto descriptor = original;
  const bool native_fringe =
      kind == AvioCoveragePipelineVariant::kFringeSample4;
  descriptor.SetSampleCount(native_fringe ? SampleCount::kCount4
                                          : SampleCount::kCount1);
  if (!native_fringe) {
    descriptor.SetDepthPixelFormat(PixelFormat::kUnknown);
    descriptor.SetStencilPixelFormat(PixelFormat::kUnknown);
  }
  descriptor.SetDepthStencilAttachmentDescriptor(std::nullopt);
  descriptor.SetStencilAttachmentDescriptors(std::nullopt, std::nullopt);
  if (kind != AvioCoveragePipelineVariant::kDirect1x) {
    const auto source = original.GetEntrypointForStage(ShaderStage::kFragment);
    const auto vertex = original.GetVertexDescriptor();
    if (context.GetBackendType() != Context::BackendType::kVulkan || !source ||
        !vertex) {
      return nullptr;
    }
    const auto prefix = kind == AvioCoveragePipelineVariant::kLayerSample4
                            ? "avio_sample4_layer_"
                        : kind == AvioCoveragePipelineVariant::kInteriorSample4
                            ? "avio_sample4_interior_"
                        : kind == AvioCoveragePipelineVariant::kFringeSample4
                            ? "avio_sample4_fringe_"
                            : "avio_sample4_";
    const auto replacement = context.GetShaderLibrary()->GetFunction(
        prefix + source->GetName(), ShaderStage::kFragment);
    if (!replacement) {
      return nullptr;
    }
    auto layout = std::make_shared<VertexDescriptor>();
    layout->SetStageInputs(vertex->GetStageInputs(), vertex->GetStageLayouts());
    for (const auto& existing : vertex->GetDescriptorSetLayouts()) {
      if (existing.binding >= kAvioSample4ControlBinding &&
          existing.binding < kAvioSample4MaskBinding + 4) {
        return nullptr;
      }
      layout->RegisterDescriptorSetLayouts(&existing, 1);
    }
    const DescriptorSetLayout control{kAvioSample4ControlBinding,
                                      DescriptorType::kStorageBuffer,
                                      ShaderStage::kFragment};
    layout->RegisterDescriptorSetLayouts(&control, 1);
    for (uint32_t index = 0; index < 4; ++index) {
      const DescriptorSetLayout mask{kAvioSample4MaskBinding + index,
                                     DescriptorType::kSampledImage,
                                     ShaderStage::kFragment};
      layout->RegisterDescriptorSetLayouts(&mask, 1);
    }
    descriptor.SetVertexDescriptor(std::move(layout));
    descriptor.AddStageEntrypoint(replacement);
    // Opaque SrcOver sources are commonly coerced to Src in their original
    // pipeline. Fractional joint coverage needs the declared SrcOver operator.
    if (kind == AvioCoveragePipelineVariant::kJointSample4 ||
        kind == AvioCoveragePipelineVariant::kLayerSample4) {
      auto output = *colour;
      output.blending_enabled = true;
      output.src_color_blend_factor = output.src_alpha_blend_factor =
          BlendFactor::kOne;
      output.dst_color_blend_factor = output.dst_alpha_blend_factor =
          BlendFactor::kOneMinusSourceAlpha;
      output.color_blend_op = output.alpha_blend_op = BlendOperation::kAdd;
      descriptor.SetColorAttachmentDescriptor(0, output);
    }
  }
  auto future =
      context.GetPipelineLibrary()->GetPipeline(std::move(descriptor), false);
  return future.IsValid() ? future.Get() : nullptr;
}
}  // namespace impeller
