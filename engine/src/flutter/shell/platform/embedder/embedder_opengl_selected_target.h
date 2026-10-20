// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_OPENGL_SELECTED_TARGET_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_OPENGL_SELECTED_TARGET_H_

#include "flutter/shell/platform/embedder/embedder_render_target_impeller.h"

namespace flutter {

// Only the negotiated coverage policy can load a retained GLES root. Legacy
// implicit MSAA has no retained multisample contents and is not a substitute.
std::unique_ptr<EmbedderRenderTarget> MakeOpenGLSelectedRenderTarget(
    FlutterBackingStore backing_store,
    const fml::closure& collect,
    const std::shared_ptr<impeller::AiksContext>& aiks_context,
    const FlutterBackingStoreConfig& config);

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_OPENGL_SELECTED_TARGET_H_
