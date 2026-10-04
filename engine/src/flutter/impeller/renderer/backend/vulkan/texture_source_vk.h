// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_TEXTURE_SOURCE_VK_H_
#define FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_TEXTURE_SOURCE_VK_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "flutter/fml/status.h"
#include "flutter/fml/unique_fd.h"
#include "impeller/core/formats.h"
#include "impeller/core/texture_descriptor.h"
#include "impeller/renderer/backend/vulkan/barrier_vk.h"
#include "impeller/renderer/backend/vulkan/formats_vk.h"
#include "impeller/renderer/backend/vulkan/shared_object_vk.h"
#include "impeller/renderer/backend/vulkan/vk.h"
#include "impeller/renderer/backend/vulkan/yuv_conversion_vk.h"

namespace impeller {

// These methods should only be used by render_pass_vk.h
struct FramebufferAndRenderPass {
  SharedHandleVK<vk::Framebuffer> framebuffer = nullptr;
  SharedHandleVK<vk::RenderPass> render_pass = nullptr;
};

/// The attachment descriptions baked into one VkRenderPass, in attachment
/// order: load/store operations and initial/final layouts included.
///
/// A framebuffer only needs render-pass *compatibility*, but
/// vkCmdBeginRenderPass executes the render pass object's own load operations
/// and layout transitions. A cached render pass is therefore replayed only for
/// an identical policy. A later full-area kLoad pass over the same subresource
/// must never inherit an earlier kClear with an UNDEFINED initial layout.
struct RenderPassPolicyVK {
  // Matches `kMaxAttachments` in render_pass_builder_vk.h (static_assert'ed
  // there): sixteen colour attachments, their resolves and one depth/stencil.
  static constexpr size_t kCapacity = 33u;
  std::array<vk::AttachmentDescription, kCapacity> attachments = {};
  size_t count = 0u;

  bool operator==(const RenderPassPolicyVK& other) const;
};

class Context;
class ExternalSemaphoreVK;
class TextureSourceVK;

/// @brief      Describes a VkSemaphore that a queue submission must wait on
///             before executing commands that reference the associated texture.
struct WaitSemaphore {
  vk::UniqueSemaphore semaphore;
  vk::PipelineStageFlags wait_stage;
  // Exact source custody until queue.submit accepts the wait or returns it.
  std::shared_ptr<const TextureSourceVK> source;
};

/// Describes an image whose exclusive queue-family ownership crosses the
/// Vulkan device boundary. Execution semaphores make writes visible, but they
/// do not transfer ownership: render passes using this capability must acquire
/// the image before access and release it after the final access.
struct ExternalImageOwnershipVK {
  uint32_t queue_family_index = VK_QUEUE_FAMILY_FOREIGN_EXT;
  vk::ImageLayout interchange_layout = vk::ImageLayout::eGeneral;
};

//------------------------------------------------------------------------------
/// @brief      Abstract base class that represents a vkImage and an
///             vkImageView.
///
///             This is intended to be used with an impeller::TextureVK. Example
///             implementations represent swapchain images, uploaded textures,
///             Android Hardware Buffer backend textures, etc...
///
class TextureSourceVK {
 public:
  virtual ~TextureSourceVK();

  //----------------------------------------------------------------------------
  /// @brief      Gets the texture descriptor for this image source.
  ///
  /// @warning    Texture descriptors from texture sources whose capabilities
  ///             are a superset of those that can be expressed with Vulkan
  ///             (like Android Hardware Buffer) are inferred. Stuff like size,
  ///             mip-counts, types is reliable. So use these descriptors as
  ///             advisory. Creating copies of texture sources from these
  ///             descriptors is usually not possible and  depends on the
  ///             allocator used.
  ///
  /// @return     The texture descriptor.
  ///
  const TextureDescriptor& GetTextureDescriptor() const;

  //----------------------------------------------------------------------------
  /// @brief      Get the image handle for this texture source.
  ///
  /// @return     The image.
  ///
  virtual vk::Image GetImage() const = 0;

  //----------------------------------------------------------------------------
  /// @brief      Retrieve the image view used for sampling/blitting/compute
  ///             with this texture source.
  ///
  /// @return     The image view.
  ///
  virtual vk::ImageView GetImageView() const = 0;

  //----------------------------------------------------------------------------
  /// @brief      Retrieve the image view used to attach a specific
  ///             subresource of this texture as a render target.
  ///
  ///             The returned view covers a single mip level and a single
  ///             array layer (or cube map face), since attachment views cannot
  ///             span multiple levels or layers.
  ///
  /// @param[in]  mip_level    The mip level to attach.
  /// @param[in]  array_layer  The array layer or cube map face to attach.
  ///
  /// @return     The render target view.
  ///
  virtual vk::ImageView GetRenderTargetView(uint32_t mip_level,
                                            uint32_t array_layer) const = 0;

  //----------------------------------------------------------------------------
  /// @brief      Encodes the layout transition `barrier` to
  ///             `barrier.cmd_buffer` for the image.
  ///
  ///             The transition is from the layout stored via
  ///             `SetLayoutWithoutEncoding` to `barrier.new_layout`.
  ///
  /// @param[in]  barrier  The barrier.
  ///
  /// @return     If the layout transition was successfully made.
  ///
  fml::Status SetLayout(const BarrierVK& barrier) const;

  //----------------------------------------------------------------------------
  /// @brief      Store the layout of the image.
  ///
  ///             This just is bookkeeping on the CPU, to actually set the
  ///             layout use `SetLayout`.
  ///
  /// @param[in]  layout  The new layout.
  ///
  /// @return     The old layout.
  ///
  vk::ImageLayout SetLayoutWithoutEncoding(vk::ImageLayout layout) const;

  //----------------------------------------------------------------------------
  /// @brief      Get the last layout assigned to the TextureSourceVK.
  ///
  ///             This value is synchronized with the GPU via SetLayout so it
  ///             may not reflect the actual layout.
  ///
  /// @return     The last known layout of the texture source.
  ///
  vk::ImageLayout GetLayout() const;

  //----------------------------------------------------------------------------
  /// @brief      When sampling from textures whose formats are not known to
  ///             Vulkan, a custom conversion is necessary to setup custom
  ///             samplers. This accessor provides this conversion if one is
  ///             present. Most texture source have none.
  ///
  /// @return     The sampler conversion.
  ///
  virtual std::shared_ptr<YUVConversionVK> GetYUVConversion() const;

  //----------------------------------------------------------------------------
  /// @brief      Determines if swapchain image. That is, an image used as the
  ///             root render target.
  ///
  /// @return     Whether or not this is a swapchain image.
  ///
  virtual bool IsSwapchainImage() const = 0;

  //----------------------------------------------------------------------------
  /// @brief      The bytes of device memory allocated for this image by the
  ///             context's allocator, or 0 for images the context does not
  ///             own (swapchain, external, or imported images).
  ///
  virtual size_t GetAllocatedByteSize() const;

  // Only allocator-owned Image/Glyph sources implement this census hook;
  // imported producer images and scratch targets never become image uploads.
  // The caller supplies frame origin after a successful upload command encode.
  virtual void RecordAvioImageUpload(bool raster_frame) const;

  virtual std::optional<ExternalImageOwnershipVK> GetExternalImageOwnership()
      const;

  virtual std::optional<WaitSemaphore> ConsumeAcquireSemaphore() const;

  /// Called only inside the owning graphics queue's SubmitLocked callback.
  /// Recording an image reference does not consume its producer dependency.
  /// Failed queue submissions return the exact semaphore before unlocking;
  /// a later submitted reader must still wait on the original producer.
  std::optional<WaitSemaphore> TakeAcquireSemaphoreForSubmit() const;
  void ReturnAcquireSemaphoreFromFailedSubmit(WaitSemaphore wait) const;

  virtual std::shared_ptr<ExternalSemaphoreVK>
  CreateRenderCompleteSignalSemaphore(
      const std::shared_ptr<Context>& context) const;

  virtual void SetRenderCompleteSyncFD(fml::UniqueFD sync_fd) const;

  virtual fml::UniqueFD TakeRenderCompleteSyncFD() const;

  // These methods should only be used by render_pass_vk.h

  /// Store the framebuffer and render pass used to render into the
  /// `(sample_count, mip_level, slice)` subresource of this texture with the
  /// exact attachment `policy` the render pass was built from.
  ///
  /// This is only called when this texture is being used as the resolve (or
  /// non-MSAA color) target of a render pass. Compatibility alone is not
  /// enough to replay a cached render pass: its load operations and initial
  /// layouts execute at vkCmdBeginRenderPass. Each distinct policy over a
  /// subresource therefore owns its own entry.
  void SetCachedFrameData(const FramebufferAndRenderPass& data,
                          SampleCount sample_count,
                          uint32_t mip_level = 0u,
                          uint32_t slice = 0u,
                          const RenderPassPolicyVK& policy = {});

  /// Retrieve the cached framebuffer and render pass for the given
  /// `(sample_count, mip_level, slice)` subresource and exact `policy`.
  ///
  /// A null `policy` returns the first entry for the subresource regardless
  /// of policy; it is for inspection only and must never select a render pass
  /// to begin. An empty `FramebufferAndRenderPass` is returned when no cached
  /// entry exists for that key. Entries are populated lazily on first use and
  /// live for the lifetime of the texture.
  FramebufferAndRenderPass GetCachedFrameData(
      SampleCount sample_count,
      uint32_t mip_level = 0u,
      uint32_t slice = 0u,
      const RenderPassPolicyVK* policy = nullptr) const;

 protected:
  const TextureDescriptor desc_;

  explicit TextureSourceVK(TextureDescriptor desc);

  // External-image sources must drop their native framebuffer/render-pass
  // cache before returning the embedder's image/view and backing-store baton.
  // Invoke only during final source destruction, after native readers retire.
  void ReleaseCachedFrameData();

 private:
  struct CachedFrameDataEntry {
    SampleCount sample_count;
    uint32_t mip_level;
    uint32_t slice;
    RenderPassPolicyVK policy;
    FramebufferAndRenderPass data;
  };
  // Linear-scanned because N is typically 1 and bounded by
  // `sample_counts * mip_count * layer_count * policies` for the rare textures
  // that are rendered to across many subresources (e.g. a fully populated cube
  // mip chain). A single-sample target that is cleared once and then loaded
  // by later passes of the same frame (the Coverage root) holds one entry per
  // load/layout policy, typically two or three.
  std::vector<CachedFrameDataEntry> frame_data_;
  mutable vk::ImageLayout layout_ = vk::ImageLayout::eUndefined;
  // No self-reference: the temporary WaitSemaphore owns the source, while the
  // source retains only a returned Vulkan handle and its original wait stage.
  mutable vk::UniqueSemaphore returned_acquire_semaphore_;
  mutable vk::PipelineStageFlags returned_acquire_stage_;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_TEXTURE_SOURCE_VK_H_
