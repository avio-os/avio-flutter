// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder_external_resource_custody.h"

#include <array>
#include <memory>

#include "gtest/gtest.h"

#if defined(SHELL_ENABLE_VULKAN)
#include "impeller/renderer/backend/vulkan/texture_source_vk.h"
#endif

namespace flutter {
namespace {

TEST(EmbedderExternalResourceCustodyTest, FinalNativeOwnerReturnsBatonInOrder) {
  std::array<int, 2> order = {};
  size_t count = 0;
  auto source = std::make_shared<EmbedderExternalResourceCustody>();
  ASSERT_TRUE(
      source->Adopt([&] { order[count++] = 1; }, [&] { order[count++] = 2; }));
  auto submitted_native_reader = source;
  source.reset();
  EXPECT_EQ(count, 0u);
  submitted_native_reader.reset();
  ASSERT_EQ(count, 2u);
  EXPECT_EQ(order, (std::array<int, 2>{1, 2}));
}

TEST(EmbedderExternalResourceCustodyTest, FailedAcquisitionKeepsOriginalGuard) {
  int collected = 0;
  {
    fml::ScopedCleanupClosure acquisition([&] { collected++; });
    // An unarmed native source cannot collect a second time on a failed
    // acquisition; the caller's original guard remains the exact owner.
    EmbedderExternalResourceCustody unarmed_source;
  }
  EXPECT_EQ(collected, 1);
}

TEST(EmbedderExternalResourceCustodyTest, CustodyCannotBeAdoptedTwice) {
  int destroyed = 0;
  int collected = 0;
  int rejected = 0;
  {
    EmbedderExternalResourceCustody owner;
    ASSERT_TRUE(owner.Adopt([&] { destroyed++; }, [&] { collected++; }));
    EXPECT_FALSE(owner.Adopt([&] { rejected++; }, [&] { rejected++; }));
  }
  EXPECT_EQ(destroyed, 1);
  EXPECT_EQ(collected, 1);
  EXPECT_EQ(rejected, 0);
}

#if defined(SHELL_ENABLE_VULKAN)
std::array<int, 5> native_order;
size_t native_count;

VKAPI_ATTR void VKAPI_CALL
DestroyTestFramebuffer(VkDevice, VkFramebuffer, const VkAllocationCallbacks*) {
  native_order[native_count++] = 1;
}
VKAPI_ATTR void VKAPI_CALL DestroyTestRenderPass(VkDevice,
                                                 VkRenderPass,
                                                 const VkAllocationCallbacks*) {
  native_order[native_count++] = 2;
}

VKAPI_ATTR void VKAPI_CALL DestroyTestSemaphore(VkDevice,
                                                VkSemaphore,
                                                const VkAllocationCallbacks*) {
  native_order[native_count++] = 3;
}

class CachedExternalSource final : public impeller::TextureSourceVK {
 public:
  CachedExternalSource() : TextureSourceVK({}) {
    custody_.Adopt([] { native_order[native_count++] = 4; },
                   [] { native_order[native_count++] = 5; });
  }
  ~CachedExternalSource() override { ReleaseCachedFrameData(); }
  impeller::vk::Image GetImage() const override { return {}; }
  impeller::vk::ImageView GetImageView() const override { return {}; }
  impeller::vk::ImageView GetRenderTargetView(uint32_t,
                                              uint32_t) const override {
    return {};
  }
  bool IsSwapchainImage() const override { return true; }

 private:
  EmbedderExternalResourceCustody custody_;
};

TEST(EmbedderExternalResourceCustodyTest,
     NativeFramebufferCacheRetiresBeforeExternalViewAndCollect) {
  // Exercise the actual Vulkan source cache and native UniqueHandle deleters
  // with a fake dispatch table; this does not create a Vulkan device or GPU.
  auto& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;
  const auto old_framebuffer = dispatch.vkDestroyFramebuffer;
  const auto old_render_pass = dispatch.vkDestroyRenderPass;
  const auto old_semaphore = dispatch.vkDestroySemaphore;
  const fml::ScopedCleanupClosure restore([&] {
    dispatch.vkDestroyFramebuffer = old_framebuffer;
    dispatch.vkDestroyRenderPass = old_render_pass;
    dispatch.vkDestroySemaphore = old_semaphore;
  });
  dispatch.vkDestroyFramebuffer = DestroyTestFramebuffer;
  dispatch.vkDestroyRenderPass = DestroyTestRenderPass;
  dispatch.vkDestroySemaphore = DestroyTestSemaphore;
  native_count = 0;
  native_order = {};
  auto source = std::make_shared<CachedExternalSource>();
  const auto device = impeller::vk::Device(reinterpret_cast<VkDevice>(1));
  impeller::vk::UniqueHandleTraits<impeller::vk::Framebuffer,
                                   VULKAN_HPP_DEFAULT_DISPATCHER_TYPE>::deleter
      deleter(device, nullptr, dispatch);
  {
    impeller::FramebufferAndRenderPass data;
    data.framebuffer = impeller::MakeSharedVK(impeller::vk::UniqueFramebuffer(
        impeller::vk::Framebuffer(reinterpret_cast<VkFramebuffer>(2)),
        deleter));
    data.render_pass = impeller::MakeSharedVK(impeller::vk::UniqueRenderPass(
        impeller::vk::RenderPass(reinterpret_cast<VkRenderPass>(3)), deleter));
    source->SetCachedFrameData(data, impeller::SampleCount::kCount4);
  }
  auto native_reader = source;
  impeller::WaitSemaphore returned_wait;
  returned_wait.semaphore = impeller::vk::UniqueSemaphore(
      impeller::vk::Semaphore(reinterpret_cast<VkSemaphore>(4)), deleter);
  source->ReturnAcquireSemaphoreFromFailedSubmit(std::move(returned_wait));
  source.reset();
  EXPECT_EQ(native_count, 0u);
  native_reader.reset();
  ASSERT_EQ(native_count, 5u);
  EXPECT_EQ(native_order[0] + native_order[1], 3);
  EXPECT_EQ(native_order[2], 3);
  EXPECT_EQ(native_order[3], 4);
  EXPECT_EQ(native_order[4], 5);
}
#endif

}  // namespace
}  // namespace flutter
