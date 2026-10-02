// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_CONTENT_CONTEXT_OPTIONS_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_CONTENT_CONTEXT_OPTIONS_H_
#include "impeller/core/formats.h"
namespace impeller {
class PipelineDescriptor;
/// Pipeline state configuration.
///
/// Each unique combination of these options requires a different pipeline state
/// object to be built. This struct is used as a key for the per-pipeline
/// variant cache.
///
/// When adding fields to this key, reliant features should take care to limit
/// the combinatorical explosion of variations. A sufficiently complicated
/// Flutter application may easily require building hundreds of PSOs in total,
/// but they shouldn't require e.g. 10s of thousands.
struct ContentContextOptions {
  enum class StencilMode : uint8_t {
    /// Turn the stencil test off. Used when drawing without stencil-then-cover.
    kIgnore,

    // Operations used for stencil-then-cover.

    /// Draw the stencil for the NonZero fill path rule.
    ///
    /// The stencil ref should always be 0 on commands using this mode.
    kStencilNonZeroFill,
    /// Draw the stencil for the EvenOdd fill path rule.
    ///
    /// The stencil ref should always be 0 on commands using this mode.
    kStencilEvenOddFill,
    /// Draw a stencil which always increments once for everything in the vertex
    /// coverage regardless of triangle overlap.
    ///
    /// The stencil ref should always be 0 on commands using this mode.
    kStencilIncrementAll,
    /// Used for draw calls which fill in the stenciled area. Intended to be
    /// used after `kStencilNonZeroFill` or `kStencilEvenOddFill` is used to set
    /// up the stencil buffer. Also cleans up the stencil buffer by resetting
    /// everything to zero.
    ///
    /// The stencil ref should always be 0 on commands using this mode.
    kCoverCompare,
    /// The opposite of `kCoverCompare`. Used for draw calls which fill in the
    /// non-stenciled area (intersection clips). Intended to be used after
    /// `kStencilNonZeroFill` or `kStencilEvenOddFill` is used to set up the
    /// stencil buffer. Also cleans up the stencil buffer by resetting
    /// everything to zero.
    ///
    /// The stencil ref should always be 0 on commands using this mode.
    kCoverCompareInverted,
  };

  SampleCount sample_count = SampleCount::kCount1;
  BlendMode blend_mode = BlendMode::kSrcOver;
  CompareFunction depth_compare = CompareFunction::kAlways;
  StencilMode stencil_mode = ContentContextOptions::StencilMode::kIgnore;
  PrimitiveType primitive_type = PrimitiveType::kTriangle;
  PixelFormat color_attachment_pixel_format = PixelFormat::kUnknown;
  bool has_depth_stencil_attachments = true;
  bool is_stencil_only = false;
  bool depth_write_enabled = false;
  bool is_for_rrect_blur_clear = false;

  constexpr uint64_t ToKey() const {
    static_assert(sizeof(sample_count) == 1);
    static_assert(sizeof(blend_mode) == 1);
    static_assert(sizeof(sample_count) == 1);
    static_assert(sizeof(depth_compare) == 1);
    static_assert(sizeof(stencil_mode) == 1);
    static_assert(sizeof(primitive_type) == 1);
    static_assert(sizeof(color_attachment_pixel_format) == 1);

    return (is_for_rrect_blur_clear ? 1llu : 0llu) << 0 |
           (is_stencil_only ? 1llu : 0llu) << 1 |
           (has_depth_stencil_attachments ? 1llu : 0llu) << 2 |
           (depth_write_enabled ? 1llu : 0llu) << 3 |
           // enums
           static_cast<uint64_t>(color_attachment_pixel_format) << 8 |
           static_cast<uint64_t>(primitive_type) << 16 |
           static_cast<uint64_t>(stencil_mode) << 24 |
           static_cast<uint64_t>(depth_compare) << 32 |
           static_cast<uint64_t>(blend_mode) << 40 |
           static_cast<uint64_t>(sample_count) << 48;
  }

  void ApplyToPipelineDescriptor(PipelineDescriptor& desc) const;
};

enum ConicalKind {
  kConical,
  kRadial,
  kStrip,
  kStripAndRadial,
};

}  // namespace impeller
#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_CONTENT_CONTEXT_OPTIONS_H_
