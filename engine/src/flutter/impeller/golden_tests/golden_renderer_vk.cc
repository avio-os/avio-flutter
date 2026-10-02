// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/golden_tests/golden_renderer_vk.h"

#include <atomic>
#include <cstring>
#include <vector>

#include "flutter/fml/native_library.h"
#include "flutter/fml/synchronization/waitable_event.h"
#include "impeller/display_list/dl_dispatcher.h"
#include "impeller/entity/vk/entity_shaders_vk.h"
#include "impeller/entity/vk/framebuffer_blend_shaders_vk.h"
#include "impeller/entity/vk/modern_shaders_vk.h"
#include "impeller/renderer/backend/vulkan/context_vk.h"
#include "impeller/renderer/blit_pass.h"
#include "impeller/renderer/command_buffer.h"
#include "impeller/renderer/command_queue.h"
#include "impeller/renderer/vk/compute_shaders_vk.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/include/encode/SkPngEncoder.h"

namespace impeller::testing {
namespace {

class PngScreenshotVK final : public Screenshot {
 public:
  PngScreenshotVK(ISize size, std::vector<uint8_t> rgba)
      : size_(size), rgba_(std::move(rgba)) {}
  const uint8_t* GetBytes() const override { return rgba_.data(); }
  size_t GetHeight() const override { return size_.height; }
  size_t GetWidth() const override { return size_.width; }
  size_t GetBytesPerRow() const override { return size_.width * 4u; }
  bool WriteToPNG(const std::string& path) const override {
    SkFILEWStream stream(path.c_str());
    const auto info = SkImageInfo::Make(
        size_.width, size_.height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    const SkPixmap pixels(info, rgba_.data(), GetBytesPerRow());
    // Screenshot bytes stay exact premultiplied renderer bytes. PNG encoding
    // performs the standard straight-alpha conversion only at export.
    return stream.isValid() && SkPngEncoder::Encode(&stream, pixels, {});
  }

 private:
  ISize size_;
  std::vector<uint8_t> rgba_;
};

class SingleSampleGoldenAllocator final : public RenderTargetAllocator {
 public:
  explicit SingleSampleGoldenAllocator(std::shared_ptr<Allocator> allocator)
      : RenderTargetAllocator(std::move(allocator)) {}
  RenderTarget CreateOffscreenMSAA(
      const Context& context,
      ISize size,
      int mip_count,
      std::string_view label,
      RenderTarget::AttachmentConfigMSAA color,
      std::optional<RenderTarget::AttachmentConfig> stencil,
      const std::shared_ptr<Texture>& existing_color,
      const std::shared_ptr<Texture>& existing_resolve,
      const std::shared_ptr<Texture>& existing_stencil,
      std::optional<PixelFormat> format) override {
    // Do not allow saveLayer/filter/Flip callers to defeat the reference's
    // single-sample contract by allocating four-sample nested targets.
    for (const auto& texture :
         {existing_color, existing_resolve, existing_stencil}) {
      if (texture &&
          texture->GetTextureDescriptor().sample_count != SampleCount::kCount1)
        return {};
    }
    auto target = RenderTargetAllocator::CreateOffscreen(
        context, size, mip_count, label,
        {StorageMode::kDevicePrivate, color.load_action, StoreAction::kStore,
         color.clear_color},
        stencil, existing_resolve ? existing_resolve : existing_color,
        existing_stencil, format);
    FML_CHECK(!target.IsValid() ||
              target.GetSampleCount() == SampleCount::kCount1);
    return target;
  }
};

}  // namespace

std::unique_ptr<Screenshot> MakeGoldenScreenshotVK(ISize size,
                                                   std::vector<uint8_t> rgba) {
  if (size.IsEmpty() || rgba.size() != static_cast<size_t>(size.Area()) * 4u) {
    return nullptr;
  }
  return std::make_unique<PngScreenshotVK>(size, std::move(rgba));
}

std::shared_ptr<Context> MakeHeadlessGoldenContextVK(AvioGoldenPolicyVK policy,
                                                     bool use_sdfs) {
  // Keep the real loader resident through Vulkan's process-wide dispatcher.
  // No GLFW window, WSI surface, display server or swapped framebuffer exists.
  static const auto loader = fml::NativeLibrary::Create("libvulkan.so.1");
  if (!loader) {
    return nullptr;
  }
  const auto get_proc = loader->ResolveFunction<PFN_vkGetInstanceProcAddr>(
      "vkGetInstanceProcAddr");
  if (!get_proc) {
    return nullptr;
  }
  ContextVK::Settings settings;
  settings.proc_address_callback = *get_proc;
  settings.enable_validation = true;
  settings.fatal_missing_validations = true;
  settings.flags.use_sdfs = use_sdfs;
  settings.pipeline_cache_access = PipelineCacheAccessVK::kDisabled;
  settings.shader_libraries_data = {
      std::make_shared<fml::NonOwnedMapping>(impeller_entity_shaders_vk_data,
                                             impeller_entity_shaders_vk_length),
      std::make_shared<fml::NonOwnedMapping>(impeller_modern_shaders_vk_data,
                                             impeller_modern_shaders_vk_length),
      std::make_shared<fml::NonOwnedMapping>(
          impeller_framebuffer_blend_shaders_vk_data,
          impeller_framebuffer_blend_shaders_vk_length),
      std::make_shared<fml::NonOwnedMapping>(
          impeller_compute_shaders_vk_data, impeller_compute_shaders_vk_length),
  };
  if (policy == AvioGoldenPolicyVK::kCoverage) {
    AvioAntialiasingConfig config;
    config.policy = AvioAntialiasingPolicy::kCoverage;
    config.layer_sample_count = 1;
    config.coverage_sample_count = 4;
    config.coverage_region_max_bytes = 8u * 1024u * 1024u;
    config.layer_region_max_bytes = 4u * 1024u * 1024u;
    settings.avio_antialiasing_config = config;
  }
  return ContextVK::Create(std::move(settings));
}

std::shared_ptr<RenderTargetAllocator> MakeGoldenAllocatorVK(
    const std::shared_ptr<Context>& context,
    AvioGoldenPolicyVK policy) {
  if (policy == AvioGoldenPolicyVK::kAliased1) {
    return std::make_shared<SingleSampleGoldenAllocator>(
        context->GetResourceAllocator());
  }
  return std::make_shared<RenderTargetAllocator>(
      context->GetResourceAllocator());
}

std::shared_ptr<Texture> RenderGoldenDisplayListVK(
    AiksContext& renderer,
    const sk_sp<flutter::DisplayList>& list,
    ISize size,
    AvioGoldenPolicyVK policy) {
  if (!renderer.IsValid() || !list) {
    return nullptr;
  }
  const auto context = renderer.GetContext();
  auto allocator = MakeGoldenAllocatorVK(context, policy);
  auto target =
      policy == AvioGoldenPolicyVK::kMsaa4
          ? allocator->CreateOffscreenMSAA(*context, size, 1)
          : allocator->CreateOffscreen(
                *context, size, 1, "Headless Golden",
                RenderTarget::kDefaultColorAttachmentConfig,
                policy == AvioGoldenPolicyVK::kCoverage
                    ? std::nullopt
                    : std::make_optional(
                          RenderTarget::kDefaultStencilAttachmentConfig));
  if (!target.IsValid() ||
      target.GetSampleCount() != (policy == AvioGoldenPolicyVK::kMsaa4
                                      ? SampleCount::kCount4
                                      : SampleCount::kCount1) ||
      (policy == AvioGoldenPolicyVK::kCoverage &&
       (target.GetDepthAttachment() || target.GetStencilAttachment()))) {
    return nullptr;
  }
  auto texture = target.GetRenderTargetTexture();
  if (!RenderToTarget(renderer.GetContentContext(), std::move(target), list,
                      Rect::MakeSize(size), true, false)) {
    return nullptr;
  }
  return texture;
}

std::unique_ptr<Screenshot> ReadGoldenTextureVK(
    const std::shared_ptr<Context>& context,
    const std::shared_ptr<Texture>& texture) {
  if (!context || !texture || !texture->IsValid()) {
    return nullptr;
  }
  const auto& descriptor = texture->GetTextureDescriptor();
  const bool bgra = descriptor.format == PixelFormat::kB8G8R8A8UNormInt;
  if (descriptor.sample_count != SampleCount::kCount1 ||
      (!bgra && descriptor.format != PixelFormat::kR8G8B8A8UNormInt)) {
    return nullptr;
  }
  DeviceBufferDescriptor buffer_descriptor;
  buffer_descriptor.storage_mode = StorageMode::kHostVisible;
  buffer_descriptor.readback = true;
  buffer_descriptor.size = descriptor.GetByteSizeOfBaseMipLevel();
  auto buffer =
      context->GetResourceAllocator()->CreateBuffer(buffer_descriptor);
  auto commands = context->CreateCommandBuffer();
  if (!buffer || !commands) {
    return nullptr;
  }
  auto blit = commands->CreateBlitPass();
  if (!blit || !blit->AddCopy(texture, buffer) || !blit->EncodeCommands()) {
    return nullptr;
  }
  struct Completion {
    fml::AutoResetWaitableEvent event;
    std::atomic<bool> completed = false;
  };
  auto completion = std::make_shared<Completion>();
  if (!context->GetCommandQueue()
           ->Submit({commands},
                    [completion, buffer](CommandBuffer::Status status) {
                      completion->completed =
                          status == CommandBuffer::Status::kCompleted;
                      completion->event.Signal();
                    })
           .ok() ||
      completion->event.WaitWithTimeout(fml::TimeDelta::FromSeconds(30)) ||
      !completion->completed) {
    return nullptr;
  }
  buffer->Invalidate();
  if (!buffer->OnGetContents()) {
    return nullptr;
  }
  std::vector<uint8_t> pixels(buffer_descriptor.size);
  std::memcpy(pixels.data(), buffer->OnGetContents(), pixels.size());
  if (bgra) {
    for (size_t i = 0; i < pixels.size(); i += 4) {
      std::swap(pixels[i], pixels[i + 2]);
    }
  }
  return MakeGoldenScreenshotVK(descriptor.size, std::move(pixels));
}

}  // namespace impeller::testing
