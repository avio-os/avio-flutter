// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_SAMPLE4_CLIP_UNIFORM_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_SAMPLE4_CLIP_UNIFORM_H_
#include <array>
#include <optional>
#include "impeller/entity/contents/sample4_clip.h"
namespace impeller {
class ContentContext;
class RenderPass;
struct alignas(16) AvioSample4ClipNodeUniform {
  Matrix lines;
  std::array<float, 4> target{};
  std::array<float, 4> raster{};
  std::array<float, 4> meta{};
};
struct alignas(16) AvioSample4ClipUniform {
  std::array<float, 4> runtime{};
  std::array<float, 4> origin{};
  std::array<AvioSample4ClipNodeUniform, 4> nodes{};
};
static_assert(sizeof(AvioSample4ClipNodeUniform) == 112);
static_assert(sizeof(AvioSample4ClipUniform) == 480);
std::optional<AvioSample4ClipUniform> MakeAvioSample4ClipUniform(
    const AvioSample4ClipDescriptor& clip,
    IPoint origin);
bool BindAvioSample4Clip(const ContentContext& renderer,
                         RenderPass& pass,
                         const AvioSample4ClipDescriptor& clip,
                         IPoint origin);
}  // namespace impeller
#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_SAMPLE4_CLIP_UNIFORM_H_
