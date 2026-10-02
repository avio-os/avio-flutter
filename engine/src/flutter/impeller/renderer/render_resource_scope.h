// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_RENDER_RESOURCE_SCOPE_H_
#define FLUTTER_IMPELLER_RENDERER_RENDER_RESOURCE_SCOPE_H_

#include "impeller/renderer/render_resource_report.h"

namespace impeller {
namespace avio_resource_scope_internal {
inline constinit thread_local bool raster_frame_active = false;
inline constinit thread_local AvioRenderResourceKind allocation_kind =
    AvioRenderResourceKind::kImageTextures;
}  // namespace avio_resource_scope_internal

// Construct only around actual raster frame work, excluding initialization,
// reporting, and IO uploads. Async creators capture this fact at their origin.
inline bool IsAvioRasterFrameActive() {
  return avio_resource_scope_internal::raster_frame_active;
}

class AvioRasterFrameScope final {
 public:
  AvioRasterFrameScope()
      : previous_(avio_resource_scope_internal::raster_frame_active) {
    avio_resource_scope_internal::raster_frame_active = true;
  }
  ~AvioRasterFrameScope() {
    avio_resource_scope_internal::raster_frame_active = previous_;
  }
  AvioRasterFrameScope(const AvioRasterFrameScope&) = delete;
  AvioRasterFrameScope& operator=(const AvioRasterFrameScope&) = delete;

 private:
  bool previous_;
};

inline AvioRenderResourceKind GetAvioResourceAllocationKind() {
  return avio_resource_scope_internal::allocation_kind;
}

// Classification is an explicit construction contract, never a label guess.
// Inner named owners (e.g. transient D/S) override the outer offscreen scope.
class AvioResourceAllocationScope final {
 public:
  explicit AvioResourceAllocationScope(AvioRenderResourceKind kind)
      : previous_(avio_resource_scope_internal::allocation_kind) {
    avio_resource_scope_internal::allocation_kind = kind;
  }
  ~AvioResourceAllocationScope() {
    avio_resource_scope_internal::allocation_kind = previous_;
  }
  AvioResourceAllocationScope(const AvioResourceAllocationScope&) = delete;
  AvioResourceAllocationScope& operator=(const AvioResourceAllocationScope&) =
      delete;

 private:
  AvioRenderResourceKind previous_;
};

}  // namespace impeller
#endif  // FLUTTER_IMPELLER_RENDERER_RENDER_RESOURCE_SCOPE_H_
