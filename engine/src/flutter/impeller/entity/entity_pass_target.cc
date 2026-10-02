// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/entity_pass_target.h"

#include "impeller/base/validation.h"
#include "impeller/core/formats.h"
#include "impeller/core/texture.h"
#include "impeller/renderer/render_resource_scope.h"

namespace impeller {

EntityPassTarget::EntityPassTarget(const RenderTarget& render_target,
                                   bool supports_read_from_resolve,
                                   bool supports_implicit_msaa)
    : target_(render_target),
      supports_read_from_resolve_(supports_read_from_resolve),
      supports_implicit_msaa_(supports_implicit_msaa) {}

std::shared_ptr<Texture> EntityPassTarget::Flip(
    const ContentContext& renderer) {
  ColorAttachment color0 = target_.GetColorAttachment(0);
  if (!color0.resolve_texture) {
    VALIDATION_LOG << "EntityPassTarget Flip should never be called for a "
                      "non-MSAA target.";
    // ...because there is never a circumstance where doing so would be
    // necessary. Unlike MSAA passes, non-MSAA passes can be trivially loaded
    // with `LoadAction::kLoad`.
    return color0.texture;
  }

  if (supports_read_from_resolve_) {
    // Just return the current resolve texture, which is safe to read in the
    // next render pass that'll resolve to `target_`.
    //
    // Note that this can only be done when MSAA is being used.
    return color0.resolve_texture;
  }

  if (!secondary_color_texture_) {
    const AvioResourceAllocationScope allocation_scope(
        AvioRenderResourceKind::kFlipTargets);
    // The second texture is allocated lazily to avoid unused allocations.
    const TextureDescriptor resolve_descriptor =
        color0.resolve_texture->GetTextureDescriptor();
    RenderTarget target;
    if (supports_implicit_msaa_) {
      // Implicit resolve renders into the single texture it samples, so the
      // secondary must come from an implicit-MSAA target as well.
      target = renderer.GetRenderTargetCache()->CreateOffscreenMSAA(
          *renderer.GetContext(), resolve_descriptor.size, 1);
    } else {
      // Only the resolve texture is swapped, so the secondary is exactly one
      // single-sample texture in the resolve format: no multisample color
      // and no depth/stencil, which would be allocated only to be dropped.
      target = renderer.GetRenderTargetCache()->CreateOffscreen(
          *renderer.GetContext(), resolve_descriptor.size, /*mip_count=*/1,
          "EntityPassTarget Secondary",
          RenderTarget::AttachmentConfig{
              .storage_mode = resolve_descriptor.storage_mode,
              .load_action = LoadAction::kDontCare,
              .store_action = StoreAction::kStore,
              .clear_color = Color::BlackTransparent(),
          },
          /*stencil_attachment_config=*/std::nullopt,
          /*existing_color_texture=*/nullptr,
          /*existing_depth_stencil_texture=*/nullptr,
          /*target_pixel_format=*/resolve_descriptor.format);
    }
    secondary_color_texture_ = target.GetRenderTargetTexture();

    if (!secondary_color_texture_) {
      return nullptr;
    }
  }

  // If the color0 resolve texture is the same as the texture, then we're
  // running on the GLES backend with implicit resolve.
  if (supports_implicit_msaa_) {
    auto new_secondary = color0.resolve_texture;
    color0.resolve_texture = secondary_color_texture_;
    color0.texture = secondary_color_texture_;
    secondary_color_texture_ = new_secondary;
  } else {
    std::swap(color0.resolve_texture, secondary_color_texture_);
  }

  target_.SetColorAttachment(color0, 0);

  // Return the previous backdrop texture, which is safe to read in the next
  // render pass that attaches `target_`.
  return secondary_color_texture_;
}

RenderTarget& EntityPassTarget::GetRenderTarget() {
  return target_;
}

bool EntityPassTarget::IsValid() const {
  return target_.IsValid();
}

void EntityPassTarget::RemoveSecondary() {
  secondary_color_texture_ = nullptr;
}

}  // namespace impeller
