// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_CLIP_OPERATION_SCOPE_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_CLIP_OPERATION_SCOPE_H_

#include "impeller/renderer/render_pass.h"

namespace impeller {

// Identify the actual logical clip writes, including their temporary winding
// and final cover. Paint-owned winding and stroke work never enters this scope.
// Nesting preserves ClipContents calling the cached-mask/quad clip helpers.
class AvioClipOperationScope {
 public:
  explicit AvioClipOperationScope(RenderPass& pass)
      : pass_(pass), previous_(pass.IsAvioClipOperation()) {
    pass_.SetAvioClipOperation(true);
  }
  ~AvioClipOperationScope() { pass_.SetAvioClipOperation(previous_); }
  AvioClipOperationScope(const AvioClipOperationScope&) = delete;
  AvioClipOperationScope& operator=(const AvioClipOperationScope&) = delete;

 private:
  RenderPass& pass_;
  const bool previous_;
};

}  // namespace impeller
#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_CLIP_OPERATION_SCOPE_H_
