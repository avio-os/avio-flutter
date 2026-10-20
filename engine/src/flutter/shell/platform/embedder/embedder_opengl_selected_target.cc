// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder_opengl_selected_target.h"

#include <cmath>
#include <limits>
#include <utility>

#include "flutter/impeller/display_list/aiks_context.h"
#include "flutter/shell/platform/embedder/avio_coverage_root_target.h"
#include "flutter/shell/platform/embedder/embedder_struct_macros.h"
#include "impeller/renderer/backend/gles/context_gles.h"
#include "impeller/renderer/backend/gles/native_coverage_gles.h"
#include "impeller/renderer/backend/gles/texture_gles.h"

namespace flutter {
namespace {

bool ValidSize(double value) {
  return std::isfinite(value) && value > 0 && std::floor(value) == value &&
         value <= std::numeric_limits<int32_t>::max();
}

// Registration already creates and validates texture FBOs on the cold raster
// membership turn. Frame acquisition never creates native attachment objects.
bool PrepareFramebuffer(impeller::TextureGLES& source,
                        impeller::ReactorGLES& reactor,
                        bool registered) {
  if (!reactor.IsValid() || !reactor.CanReactOnCurrentThread()) {
    return false;
  }
  const auto& gl = reactor.GetProcTable();
  if (gl.GetError() != GL_NO_ERROR) {
    return false;
  }
  const auto fbo = source.GetFBO();
  return fbo &&
         (registered || impeller::ValidateCoverageFramebufferGLES(gl, *fbo)) &&
         gl.GetError() == GL_NO_ERROR;
}

}  // namespace

std::unique_ptr<EmbedderRenderTarget> MakeOpenGLSelectedRenderTarget(
    FlutterBackingStore backing_store,
    const fml::closure& collect,
    const std::shared_ptr<impeller::AiksContext>& aiks_context,
    const FlutterBackingStoreConfig& config) {
  const auto* store = &backing_store;
  const auto* synchronization =
      SAFE_ACCESS(store, open_gl_synchronization, nullptr);
  if (backing_store.type != kFlutterBackingStoreTypeOpenGL ||
      !synchronization ||
      !STRUCT_HAS_MEMBER(synchronization, export_render_complete_sync_fd) ||
      !synchronization->export_render_complete_sync_fd || !aiks_context ||
      !aiks_context->GetContext() || !aiks_context->GetContext()->IsValid() ||
      aiks_context->GetContext()->GetBackendType() !=
          impeller::Context::BackendType::kOpenGLES ||
      !aiks_context->GetContext()->GetAvioAntialiasingConfig().UsesCoverage() ||
      !ValidSize(config.size.width) || !ValidSize(config.size.height)) {
    return nullptr;
  }
  auto reactor =
      impeller::ContextGLES::Cast(*aiks_context->GetContext()).GetReactor();
  impeller::TextureDescriptor desc;
  desc.format = impeller::PixelFormat::kR8G8B8A8UNormInt;
  desc.size = {static_cast<int32_t>(config.size.width),
               static_cast<int32_t>(config.size.height)};
  desc.storage_mode = impeller::StorageMode::kDevicePrivate;
  desc.usage = impeller::TextureUsage::kRenderTarget;
  desc.sample_count = impeller::SampleCount::kCount1;
  std::shared_ptr<impeller::TextureGLES> source;
  std::shared_ptr<const impeller::ContextGLES::ExternalTarget> registered;
  VoidCallback destruction = nullptr;
  void* destruction_data = nullptr;
  switch (backing_store.open_gl.type) {
    case kFlutterOpenGLTargetTypeFramebuffer: {
      const auto& fbo = backing_store.open_gl.framebuffer;
      if (fbo.target != GL_RGBA8 || fbo.name == 0u) {
        return nullptr;
      }
      source = impeller::TextureGLES::WrapFBO(reactor, desc, fbo.name);
      destruction = fbo.destruction_callback;
      destruction_data = fbo.user_data;
      break;
    }
    case kFlutterOpenGLTargetTypeTexture: {
      const auto& texture = backing_store.open_gl.texture;
      if (texture.target != GL_TEXTURE_2D || texture.format != GL_RGBA8 ||
          texture.width != static_cast<size_t>(desc.size.width) ||
          texture.height != static_cast<size_t>(desc.size.height)) {
        return nullptr;
      }
      registered = impeller::ContextGLES::Cast(*aiks_context->GetContext())
                       .AcquireExternalTarget(
                           SAFE_ACCESS(synchronization, selected_target_id, 0),
                           texture.name, desc.size);
      if (!registered) {
        return nullptr;
      }
      source = impeller::TextureGLES::WrapFBO(reactor, desc,
                                              registered->framebuffer);
      destruction = texture.destruction_callback;
      destruction_data = texture.user_data;
      break;
    }
    default:
      return nullptr;
  }
  if (!source) {
    return nullptr;
  }

  const auto* contents = SAFE_ACCESS(store, content_state, nullptr);
  const bool preserved =
      contents && STRUCT_HAS_MEMBER(contents, existing_damage) &&
      contents->preserved_contents && contents->existing_damage;
  auto create_target = [source, reactor, preserved,
                        context = aiks_context->GetContext(),
                        is_registered = bool(registered)] {
    if (!context->IsValid() ||
        !PrepareFramebuffer(*source, *reactor, is_registered)) {
      return std::unique_ptr<impeller::RenderTarget>{};
    }
    return MakeAvioCoverageRootTarget(source, preserved);
  };
  auto take_completion = [source, reactor, context = aiks_context->GetContext(),
                          callback =
                              synchronization->export_render_complete_sync_fd,
                          user_data = synchronization->user_data,
                          attempted = false]() mutable -> fml::UniqueFD {
    // A callback cannot certify queued commands that have not reached the GL
    // context. React issues commands only; it does not wait for GPU completion.
    if (std::exchange(attempted, true)) {
      return {};
    }
    auto& gl_context = impeller::ContextGLES::Cast(*context);
    if (!context->IsValid() || !source->IsValid() ||
        !reactor->CanReactOnCurrentThread() || !reactor->React() ||
        !context->IsValid() || !reactor->IsValid() ||
        !reactor->CanReactOnCurrentThread() ||
        reactor->GetProcTable().GetError() != GL_NO_ERROR) {
      gl_context.MarkExternalCompletionUnobservable();
      return {};
    }
    fml::UniqueFD completion(callback(user_data));
    if (!completion.is_valid()) {
      gl_context.MarkExternalCompletionUnobservable();
    }
    return completion;
  };
  auto result = std::make_unique<EmbedderRenderTargetImpeller>(
      backing_store, aiks_context, DlISize(desc.size), std::move(create_target),
      fml::closure{}, fml::closure{}, std::move(take_completion),
      /*supports_partial_msaa=*/true,
      /*requires_render_complete_sync_fd=*/true);
  // The acquisition guard still owns collection until the result exists.
  // The actual source follows render-pass references, including queued work.
  const bool adopted = source->AdoptExternalResourceCallback(
      [destruction, destruction_data, collect, registered]() mutable {
        // The collection receipt may immediately enqueue membership retirement.
        // Release this last reader before publishing that receipt.
        registered.reset();
        if (destruction) {
          destruction(destruction_data);
        }
        collect();
      });
  FML_CHECK(adopted);
  return result;
}

}  // namespace flutter
