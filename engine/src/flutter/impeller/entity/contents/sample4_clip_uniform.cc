// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "impeller/entity/contents/sample4_clip_uniform.h"

#include <cmath>
#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/contents/sample4_clip_pipeline.h"
#include "impeller/renderer/render_pass.h"
#include "impeller/renderer/sampler_library.h"
namespace impeller {
std::optional<AvioSample4ClipUniform> MakeAvioSample4ClipUniform(
    const AvioSample4ClipDescriptor& clip,
    IPoint origin) {
  if (clip.parent || clip.count == 0 || clip.count > clip.nodes.size()) {
    return std::nullopt;
  }
  AvioSample4ClipUniform result;
  result.runtime[0] = static_cast<float>(clip.count);
  result.origin = {static_cast<float>(origin.x), static_cast<float>(origin.y),
                   0, 0};
  for (size_t i = 0; i < clip.count; ++i) {
    const auto& node = clip.nodes[i];
    auto& output = result.nodes[i];
    if (node.quad.has_value() == node.mask.has_value()) {
      return std::nullopt;
    }
    if (node.operation != ClipOperation::kIntersect &&
        node.operation != ClipOperation::kDifference) {
      return std::nullopt;
    }
    output.meta[1] = node.operation == ClipOperation::kDifference ? 1.f : 0.f;
    if (node.quad) {
      output.lines = node.quad->GetLineParameters();
    } else {
      const auto& mask = *node.mask;
      if (!mask.lease || !mask.lease->IsValid() ||
          mask.lease->GetKind() != AvioCoverageRegion::Kind::kMask ||
          !mask.target_rect.IsFinite()) {
        return std::nullopt;
      }
      const auto content = mask.lease->GetContentRect();
      const auto origin = mask.target_rect.GetOrigin();
      if (origin.x != std::floor(origin.x) ||
          origin.y != std::floor(origin.y) ||
          mask.target_rect.GetSize() != Size(content.GetSize())) {
        return std::nullopt;
      }
      output.meta[0] = 1.f;
      output.target = {origin.x, origin.y, mask.target_rect.GetWidth(),
                       mask.target_rect.GetHeight()};
      output.raster = {static_cast<float>(content.GetLeft()),
                       static_cast<float>(content.GetTop()), 0, 0};
    }
  }
  return result;
}
bool BindAvioSample4Clip(const ContentContext& renderer,
                         RenderPass& pass,
                         const AvioSample4ClipDescriptor& clip,
                         IPoint origin) {
  auto uniform = MakeAvioSample4ClipUniform(clip, origin);
  auto* buffer = renderer.GetAvioContinuousDataBuffer();
  auto region = renderer.GetContext()->GetAvioCoverageRegion();
  if (!uniform || !buffer || !region || !region->CachesNativeMasks()) {
    return false;
  }
  const ShaderUniformSlot control{"AvioSample4ClipControl", 0, 0,
                                  kAvioSample4ControlBinding};
  auto view = buffer->Emplace(
      &*uniform, sizeof(*uniform),
      std::max(
          alignof(AvioSample4ClipUniform),
          renderer.GetDeviceCapabilities().GetMinimumStorageBufferAlignment()));
  if (!view ||
      !pass.BindResource(ShaderStage::kFragment, DescriptorType::kStorageBuffer,
                         control, nullptr, std::move(view))) {
    return false;
  }
  SamplerDescriptor sampler_descriptor;
  auto sampler = renderer.GetContext()->GetSamplerLibrary()->GetSampler(
      sampler_descriptor);
  if (!sampler)
    return false;
  for (size_t i = 0; i < clip.nodes.size(); ++i) {
    auto texture =
        i < clip.count && clip.nodes[i].mask
            ? clip.nodes[i]
                  .mask->lease->GetRenderTarget()
                  .GetColorAttachment(0)
                  .texture
            : region->GetCoverageAtlasTarget().GetColorAttachment(0).texture;
    const SampledImageSlot slot{
        "avio_sample4_mask", 0, 0,
        kAvioSample4MaskBinding + static_cast<uint32_t>(i)};
    if (!texture || !pass.BindResource(ShaderStage::kFragment,
                                       DescriptorType::kSampledImage, slot,
                                       nullptr, std::move(texture), sampler))
      return false;
  }
  return true;
}
}  // namespace impeller
