// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_COVERAGE_CLIP_MASK_FLATTENER_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_COVERAGE_CLIP_MASK_FLATTENER_H_

#include "impeller/entity/contents/sample4_clip.h"

namespace impeller {
class CommandBuffer;

// Scratch stores a binary cumulative exclusion in D32, not the original clip
// stack ordinal. S8 is reset by stencil-and-cover for every individual shape.
inline constexpr uint32_t kClipMaskReplayLocalDepth = 0;

// Pure admission and coordinate transform used by the native executor. Invalid
// or unrepresented parents never become an all-covered mask.
PreparedFillMaskStatus ValidateClipMaskTile(const AvioSample4ClipRecipe& recipe,
                                            IRect raster,
                                            ISize scratch_size);
Matrix ClipMaskReplayTransform(ISize logical_pass_size,
                               ISize scratch_size,
                               IPoint raster_origin);

// Encode complete oldest-to-newest native4 clip replay outside all caller
// passes. The caller must encode its consumer before requesting the next tile:
// the returned logical lease pins atlas contents until that encode, while the
// command buffer independently retains/tracks physical images until completion.
// The exclusive source is the warm R8 native4 scratch, not a region from the
// persistent source-mask atlas: no copy/resolve or cache capacity is required.
// kDeferred means a preclaim failed before any scratch/atlas mutation. No new
// texture, lease control block, GPU wait or resolve-alpha fallback is allowed.
AcquiredCoverageMask FlattenClipTile(
    const ContentContext& renderer,
    const std::shared_ptr<CommandBuffer>& commands,
    const std::shared_ptr<const AvioSample4ClipRecipe>& recipe,
    IRect raster);

}  // namespace impeller
#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_COVERAGE_CLIP_MASK_FLATTENER_H_
