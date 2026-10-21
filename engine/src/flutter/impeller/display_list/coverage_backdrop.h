// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_DISPLAY_LIST_COVERAGE_BACKDROP_H_
#define FLUTTER_IMPELLER_DISPLAY_LIST_COVERAGE_BACKDROP_H_

#include <optional>

#include "impeller/renderer/snapshot.h"

namespace impeller {
class ContentContext;
class RenderTarget;

namespace canvas_internal {
// Captures an immutable logical prefix after the caller has ended its pass.
// required_input and opaque_prefix are relative to the source content origin;
// the resulting Snapshot maps physical leased texels back into that space.
// Uses the existing coverage layer bank and preserves queued logical custody.
std::optional<Snapshot> CaptureCoverageBackdrop(
    const ContentContext& renderer,
    const RenderTarget& source,
    Rect required_input,
    const std::optional<Rect>& opaque_prefix);
}  // namespace canvas_internal
}  // namespace impeller

#endif  // FLUTTER_IMPELLER_DISPLAY_LIST_COVERAGE_BACKDROP_H_
