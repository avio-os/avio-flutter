// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_SWAPCHAIN_SWAPCHAIN_TRANSIENTS_VK_H_
#define FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_SWAPCHAIN_SWAPCHAIN_TRANSIENTS_VK_H_

#include <mutex>

#include "impeller/core/texture.h"
#include "impeller/core/texture_descriptor.h"
#include "impeller/renderer/context.h"

namespace impeller {

//------------------------------------------------------------------------------
/// @brief      Resources, meant to be memoized by the texture descriptor of the
///             wrapped swapchain images, that are intuitively cheap to create
///             but have been observed to be time consuming to construct on some
///             Vulkan drivers. This includes the device-transient MSAA and
///             depth-stencil textures.
///
///             The same textures are used for all swapchain images. When
///             reused across multiple render passes, the contents of these
///             attachments are not preserved between passes — color uses
///             clear+resolve semantics and depth/stencil uses clear+dont-care
///             via `LoadAction::kClear` and `kDeviceTransient` storage.
///
///             The lazy texture initialization is mutex-guarded so a single
///             instance may be safely shared across multiple render targets
///             that draw on the same thread (e.g. multiple Flutter views in
///             one frame, or the next frame of a view while the previous one
///             is still executing on the GPU).
///
///             Sharing invariant: every render pass that uses these
///             attachments is recorded on the raster thread and submitted to
///             the context's one graphics queue, in raster-thread order
///             (`CommandQueueVK::Submit` → `ContextVK::GetGraphicsQueue`).
///             Within that queue the render pass's incoming subpass
///             dependency orders this pass's color and depth/stencil clears
///             after earlier passes' attachment writes
///             (`RenderPassBuilderVK::Build`, `deps[0]`), and each pass
///             clears both attachments, so no contents cross a pass boundary.
///             Framebuffers are cached on the per-target resolve image, not on
///             these attachments, so sharing never reuses a stale framebuffer.
///
///             Rendering with the same instance from a second queue or a
///             second raster thread is **not** supported — the GPU would alias
///             the attachments. If parallel render submission is ever
///             introduced, the pool key must gain the submitting queue.
///
class SwapchainTransientsVK {
 public:
  explicit SwapchainTransientsVK(std::weak_ptr<Context> context,
                                 TextureDescriptor desc,
                                 bool enable_msaa);

  ~SwapchainTransientsVK();

  SwapchainTransientsVK(const SwapchainTransientsVK&) = delete;

  SwapchainTransientsVK& operator=(const SwapchainTransientsVK&) = delete;

  const std::weak_ptr<Context>& GetContext() const;

  bool IsMSAAEnabled() const;

  bool IsIdle() const;

  /// The device memory allocated for the attachments created so far.
  size_t GetAllocatedByteSize() const;

  const std::shared_ptr<Texture>& GetMSAATexture();

  const std::shared_ptr<Texture>& GetDepthStencilTexture();

 private:
  std::weak_ptr<Context> context_;
  const TextureDescriptor desc_;
  const bool enable_msaa_;
  // Lazy-init mutex. A `std::once_flag` would be wrong here: a transient
  // creation failure (e.g. expired context, allocator OOM) must remain
  // retryable on the next call rather than locking in a null result.
  mutable std::mutex init_mutex_;
  std::shared_ptr<Texture> cached_msaa_texture_;
  std::shared_ptr<Texture> cached_depth_stencil_;

  std::shared_ptr<Texture> CreateMSAATexture() const;

  std::shared_ptr<Texture> CreateDepthStencilTexture() const;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_SWAPCHAIN_SWAPCHAIN_TRANSIENTS_VK_H_
