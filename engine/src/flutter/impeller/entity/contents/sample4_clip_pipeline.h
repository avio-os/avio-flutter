// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_SAMPLE4_CLIP_PIPELINE_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_SAMPLE4_CLIP_PIPELINE_H_
#include <memory>
#include "impeller/renderer/pipeline.h"
#include "impeller/renderer/pipeline_descriptor.h"
namespace impeller {
class Context;
enum class AvioCoveragePipelineVariant {
  kContinuous,
  kDirect1x,
  kJointSample4,
  kLayerSample4,
  kInteriorSample4,
  kFringeSample4,
  kCount,
};
inline constexpr uint32_t kAvioSample4ControlBinding = 93;
inline constexpr uint32_t kAvioSample4MaskBinding = 94;
std::shared_ptr<Pipeline<PipelineDescriptor>> CreateAvioCoveragePipelineVariant(
    const Context& context,
    const PipelineDescriptor& original,
    AvioCoveragePipelineVariant kind);
}  // namespace impeller
#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_SAMPLE4_CLIP_PIPELINE_H_
