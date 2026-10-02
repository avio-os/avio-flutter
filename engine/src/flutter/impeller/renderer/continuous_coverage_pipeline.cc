// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/continuous_coverage_pipeline.h"

#include "impeller/renderer/pipeline_library.h"
#include "impeller/renderer/shader_function.h"
#include "impeller/renderer/shader_library.h"
#include "impeller/renderer/vertex_descriptor.h"

namespace impeller {

AvioContinuousControl MakeAvioContinuousControl(
    const ColorAttachmentDescriptor& original,
    uint32_t clip_count,
    IPoint raster_origin,
    const AvioContinuousPrimitive* own_geometry) {
  return {
      .runtime = {static_cast<float>(clip_count), own_geometry ? 1.f : 0.f, 0,
                  0},
      .origin = {static_cast<float>(raster_origin.x),
                 static_cast<float>(raster_origin.y), 0, 0},
      .factors = {static_cast<float>(original.src_color_blend_factor),
                  static_cast<float>(original.dst_color_blend_factor),
                  static_cast<float>(original.src_alpha_blend_factor),
                  static_cast<float>(original.dst_alpha_blend_factor)},
      .operations = {static_cast<float>(original.color_blend_op),
                     static_cast<float>(original.alpha_blend_op),
                     original.blending_enabled ? 1.f : 0.f, 0},
      .own_geometry = own_geometry ? *own_geometry : AvioContinuousPrimitive{},
  };
}

std::shared_ptr<Pipeline<PipelineDescriptor>> GetAvioContinuousPipeline(
    const Context& context,
    const PipelineDescriptor& original) {
  const auto* colour = original.GetColorAttachmentDescriptor(0);
  const auto function = original.GetEntrypointForStage(ShaderStage::kFragment);
  const auto vertex = original.GetVertexDescriptor();
  if (context.GetBackendType() != Context::BackendType::kVulkan ||
      original.GetSampleCount() != SampleCount::kCount4 || !colour ||
      colour->write_mask == ColorWriteMaskBits::kNone || !function || !vertex) {
    return nullptr;
  }
  const auto replacement = context.GetShaderLibrary()->GetFunction(
      "avio_continuous_" + function->GetName(), ShaderStage::kFragment);
  if (!replacement) {
    return nullptr;
  }
  auto descriptor = original;
  auto layout = std::make_shared<VertexDescriptor>();
  layout->SetStageInputs(vertex->GetStageInputs(), vertex->GetStageLayouts());
  for (const auto& existing : vertex->GetDescriptorSetLayouts()) {
    if (existing.binding == kAvioContinuousControlBinding ||
        existing.binding == kAvioContinuousUniformBinding ||
        existing.binding == kAvioContinuousDestinationBinding) {
      return nullptr;
    }
    layout->RegisterDescriptorSetLayouts(&existing, 1);
  }
  const DescriptorSetLayout additional[] = {
      {kAvioContinuousControlBinding, DescriptorType::kStorageBuffer,
       ShaderStage::kFragment},
      {kAvioContinuousUniformBinding, DescriptorType::kStorageBuffer,
       ShaderStage::kFragment},
      {kAvioContinuousDestinationBinding, DescriptorType::kSampledImage,
       ShaderStage::kFragment},
  };
  layout->RegisterDescriptorSetLayouts(additional, std::size(additional));
  descriptor.SetVertexDescriptor(std::move(layout));
  descriptor.AddStageEntrypoint(replacement);
  auto output = *colour;
  output.blending_enabled = false;
  descriptor.SetColorAttachmentDescriptor(0, output);
  descriptor.SetLabel(std::string(original.GetLabel()) +
                      " Continuous Coverage");
  auto future =
      context.GetPipelineLibrary()->GetPipeline(std::move(descriptor), false);
  return future.IsValid() ? future.Get() : nullptr;
}

}  // namespace impeller
