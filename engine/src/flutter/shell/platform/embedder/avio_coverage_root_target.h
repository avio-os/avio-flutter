// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_AVIO_COVERAGE_ROOT_TARGET_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_AVIO_COVERAGE_ROOT_TARGET_H_

#include <memory>

#include "impeller/core/texture.h"
#include "impeller/renderer/render_target.h"

namespace flutter {

// The host owns this image. Coverage masks, rather than root multisample or
// depth/stencil attachments, provide geometric antialiasing under the coverage
// policy. Keep its source alive through the normal render-target/GPU custody.
inline std::unique_ptr<impeller::RenderTarget> MakeAvioCoverageRootTarget(
    std::shared_ptr<impeller::Texture> imported_color,
    bool preserved_contents) {
  if (!imported_color || !imported_color->IsValid() ||
      imported_color->GetTextureDescriptor().sample_count !=
          impeller::SampleCount::kCount1) {
    return nullptr;
  }
  impeller::ColorAttachment color;
  color.texture = std::move(imported_color);
  color.load_action = preserved_contents ? impeller::LoadAction::kLoad
                                         : impeller::LoadAction::kClear;
  color.store_action = impeller::StoreAction::kStore;
  color.clear_color = impeller::Color::BlackTransparent();
  auto target = std::make_unique<impeller::RenderTarget>();
  target->SetColorAttachment(color, 0u);
  if (!target->IsValid()) {
    return nullptr;
  }
  return target;
}

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_EMBEDDER_AVIO_COVERAGE_ROOT_TARGET_H_
