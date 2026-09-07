// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/tests/embedder_test_backingstore_producer_vulkan.h"

#include <poll.h>
#include <cerrno>

#include "flutter/fml/logging.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/gpu/ganesh/GrBackendSurface.h"
#include "third_party/skia/include/gpu/ganesh/SkSurfaceGanesh.h"
#include "third_party/skia/include/gpu/ganesh/vk/GrVkBackendSurface.h"
#include "third_party/skia/include/gpu/ganesh/vk/GrVkTypes.h"
#include "third_party/skia/include/gpu/vk/VulkanMutableTextureState.h"

namespace flutter::testing {

namespace {
struct UserData {
  ~UserData() {
    surface.reset();
    if (vulkan_context) {
      vulkan_context->DestroyImageView(image_view);
    }
    delete image;
  }

  sk_sp<SkSurface> surface;
  GrBackendTexture backend_texture;
  FlutterVulkanImage* image;
  VkImageView image_view;
  fml::RefPtr<TestVulkanContext> vulkan_context;
};
}  // namespace

EmbedderTestBackingStoreProducerVulkan::EmbedderTestBackingStoreProducerVulkan(
    fml::RefPtr<TestVulkanContext> test_vulkan_context,
    RenderTargetType type)
    : EmbedderTestBackingStoreProducer(
          test_vulkan_context->GetGrDirectContext(),
          type),
      test_vulkan_context_(std::move(test_vulkan_context)) {}

EmbedderTestBackingStoreProducerVulkan::
    ~EmbedderTestBackingStoreProducerVulkan() = default;

bool EmbedderTestBackingStoreProducerVulkan::Create(
    const FlutterBackingStoreConfig* config,
    FlutterBackingStore* backing_store_out) {
  auto surface_size = DlISize(config->size.width, config->size.height);
  auto optional_image = test_vulkan_context_->CreateImage(surface_size);
  if (!optional_image.has_value()) {
    FML_LOG(ERROR) << "Could not create Vulkan image.";
    return false;
  }
  TestVulkanImage* test_image = new TestVulkanImage(std::move(*optional_image));

  GrVkImageInfo image_info = {
      .fImage = test_image->GetImage(),
      .fImageTiling = VK_IMAGE_TILING_OPTIMAL,
      .fImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .fFormat = VK_FORMAT_R8G8B8A8_UNORM,
      .fImageUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT,
      .fSampleCount = 1,
      .fLevelCount = 1,
      .fCurrentQueueFamily = test_vulkan_context_->GetGraphicsQueueIndex(),
  };
  auto backend_texture = GrBackendTextures::MakeVk(
      surface_size.width, surface_size.height, image_info);

  SkSurfaceProps surface_properties(0, kUnknown_SkPixelGeometry);

  SkSurfaces::TextureReleaseProc release_vktexture = [](void* user_data) {
    delete reinterpret_cast<TestVulkanImage*>(user_data);
  };

  sk_sp<SkSurface> surface = SkSurfaces::WrapBackendTexture(
      context_.get(),            // context
      backend_texture,           // back-end texture
      kTopLeft_GrSurfaceOrigin,  // surface origin
      1,                         // sample count
      kRGBA_8888_SkColorType,    // color type
      SkColorSpace::MakeSRGB(),  // color space
      &surface_properties,       // surface properties
      release_vktexture,         // texture release proc
      test_image                 // release context
  );

  if (!surface) {
    FML_LOG(ERROR) << "Could not create Skia surface from Vulkan image.";
    return false;
  }
  backing_store_out->type = kFlutterBackingStoreTypeVulkan;

  auto image = new FlutterVulkanImage{
      .struct_size = sizeof(FlutterVulkanImage),
      .image = reinterpret_cast<uint64_t>(image_info.fImage),
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .has_external_queue_family_ownership = false,
      .external_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
  };
  backing_store_out->vulkan.image = image;

  // Create a VkImageView for Impeller use via the proc table.
  VkImageView image_view = test_vulkan_context_->CreateImageView(
      image_info.fImage, VK_FORMAT_R8G8B8A8_UNORM,
      SkISize::Make(surface_size.width, surface_size.height));
  if (image_view == VK_NULL_HANDLE) {
    delete image;
    FML_LOG(ERROR) << "Could not create VkImageView for test backing store.";
    return false;
  }
  backing_store_out->vulkan.image_view = reinterpret_cast<uint64_t>(image_view);

  // Collect all allocated resources in the destruction_callback.
  // Hold a ref to the TestVulkanContext so its device outlives the image view.
  {
    auto user_data = new UserData{
        .surface = surface,
        .backend_texture = backend_texture,
        .image = image,
        .image_view = image_view,
        .vulkan_context = test_vulkan_context_,
    };
    backing_store_out->user_data = user_data;
    backing_store_out->vulkan.user_data = user_data;
    backing_store_out->vulkan.destruction_callback = [](void* user_data) {
      delete reinterpret_cast<UserData*>(user_data);
    };
  }

  return true;
}

bool EmbedderTestBackingStoreProducerVulkan::PrepareForExternalRendering(
    const FlutterBackingStore* backing_store) {
  auto* data = reinterpret_cast<UserData*>(backing_store->user_data);
  auto context = data->vulkan_context->GetGrDirectContext();
  const uint32_t queue = data->vulkan_context->GetGraphicsQueueIndex();
  // Invalidate cached snapshots before a foreign writer changes the image.
  data->surface->notifyContentWillChange(SkSurface::kRetain_ContentChangeMode);
  const auto state =
      skgpu::MutableTextureStates::MakeVulkan(VK_IMAGE_LAYOUT_GENERAL, queue);
  context->flush(data->surface.get(), GrFlushInfo{}, &state);
  if (!context->submit(GrSyncCpu::kYes)) {
    return false;
  }
  data->image->has_external_queue_family_ownership = true;
  data->image->external_queue_family_index = queue;
  return true;
}

bool EmbedderTestBackingStoreProducerVulkan::CompleteExternalRendering(
    const FlutterBackingStore* backing_store,
    int render_complete_sync_fd) {
  if (render_complete_sync_fd >= 0) {
    pollfd completion{
        .fd = render_complete_sync_fd, .events = POLLIN, .revents = 0};
    int result;
    do {
      result = poll(&completion, 1, 5000);
    } while (result < 0 && errno == EINTR);
    if (result <= 0 || (completion.revents & POLLIN) == 0 ||
        (completion.revents & (POLLERR | POLLNVAL)) != 0) {
      return false;
    }
  }
  auto* data = reinterpret_cast<UserData*>(backing_store->user_data);
  // The engine's completed release leaves this image in GENERAL. Notify Skia
  // of that actual state; do not encode a transition from its stale old layout.
  data->backend_texture.setMutableState(skgpu::MutableTextureStates::MakeVulkan(
      VK_IMAGE_LAYOUT_GENERAL, data->vulkan_context->GetGraphicsQueueIndex()));
  return true;
}

sk_sp<SkSurface> EmbedderTestBackingStoreProducerVulkan::GetSurface(
    const FlutterBackingStore* backing_store) const {
  UserData* user_data = reinterpret_cast<UserData*>(backing_store->user_data);
  return user_data->surface;
}

sk_sp<SkImage> EmbedderTestBackingStoreProducerVulkan::MakeImageSnapshot(
    const FlutterBackingStore* backing_store) const {
  UserData* user_data = reinterpret_cast<UserData*>(backing_store->user_data);
  return user_data->surface->makeImageSnapshot();
}

}  // namespace flutter::testing
