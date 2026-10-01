// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Synchronization-validation proof for one transient attachment set shared by
// same-sized root targets (Avio patch 46a). These are Vulkan playground tests:
// run them with
//
//   VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
//
// (the Vulkan playground already enables VK_LAYER_KHRONOS_validation), for
// example on SwiftShader with --use_swiftshader. Without that variable they
// skip, because nothing would check for hazards.
//
// Two root passes that share one MSAA color and depth/stencil set touch the
// shared attachments only through their load and store operations and layout
// transitions: each pass clears both attachments on load and discards them on
// store. Draws inside a pass (clips, stencil fills) come after its load
// operation in the same subpass, so the hazard between passes is fully
// exercised by passes without draws.

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "flutter/fml/synchronization/count_down_latch.h"
#include "flutter/testing/testing.h"
#include "gtest/gtest.h"
#include "impeller/base/validation.h"
#include "impeller/core/formats.h"
#include "impeller/core/texture_descriptor.h"
#include "impeller/playground/playground_test.h"
#include "impeller/renderer/backend/vulkan/command_buffer_vk.h"
#include "impeller/renderer/backend/vulkan/context_vk.h"
#include "impeller/renderer/backend/vulkan/render_pass_builder_vk.h"
#include "impeller/renderer/backend/vulkan/swapchain/surface_vk.h"
#include "impeller/renderer/backend/vulkan/swapchain/swapchain_transients_vk.h"
#include "impeller/renderer/backend/vulkan/swapchain/transients_pool_vk.h"
#include "impeller/renderer/backend/vulkan/texture_vk.h"
#include "impeller/renderer/command_buffer.h"
#include "impeller/renderer/render_pass.h"
#include "impeller/renderer/render_target.h"

namespace impeller {
namespace testing {

using RendererTest = PlaygroundTest;

namespace {

constexpr const char* kSyncValidationHint =
    "Run with "
    "VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_"
    "EXT to check synchronization.";

bool SyncValidationRequested() {
  const char* enables = std::getenv("VK_LAYER_ENABLES");
  return enables != nullptr &&
         std::strstr(enables,
                     "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_"
                     "VALIDATION_EXT") != nullptr;
}

// While alive, counts synchronization hazards the validation layer reports.
// Any other validation message is a test failure.
class ScopedSyncHazardCounter {
 public:
  ScopedSyncHazardCounter() {
    ImpellerValidationErrorsSetCallback(
        [this](const char* message, const char* file, int line) -> bool {
          if (std::strstr(message, "SYNC-HAZARD") != nullptr) {
            hazards_++;
            return true;
          }
          ADD_FAILURE_AT(file, line) << message;
          return true;
        });
  }

  ~ScopedSyncHazardCounter() {
    // Validation errors after this scope fail the test, as in PlaygroundTest.
    ImpellerValidationErrorsSetCallback(
        [](const char* message, const char* file, int line) -> bool {
          ADD_FAILURE_AT(file, line) << message;
          return true;
        });
  }

  size_t hazards() const { return hazards_.load(); }

 private:
  std::atomic<size_t> hazards_ = 0u;

  ScopedSyncHazardCounter(const ScopedSyncHazardCounter&) = delete;

  ScopedSyncHazardCounter& operator=(const ScopedSyncHazardCounter&) = delete;
};

TextureDescriptor RootTargetDescriptor(const Context& context) {
  TextureDescriptor desc;
  desc.type = TextureType::kTexture2D;
  desc.format = context.GetCapabilities()->GetDefaultColorFormat();
  desc.size = ISize(256, 192);
  desc.usage = TextureUsage::kRenderTarget;
  desc.storage_mode = StorageMode::kDevicePrivate;
  desc.sample_count = SampleCount::kCount1;
  return desc;
}

bool SubmitAndWait(const std::shared_ptr<Context>& context,
                   const std::shared_ptr<CommandBuffer>& buffer) {
  auto latch = std::make_shared<fml::CountDownLatch>(1u);
  if (!context->GetCommandQueue()
           ->Submit(
               {buffer},
               [latch](CommandBuffer::Status status) { latch->CountDown(); })
           .ok()) {
    return false;
  }
  latch->Wait();
  return true;
}

// The render pass RenderPassBuilderVK built for this target before patch 46a:
// the same attachments, subpass and dependencies, except that the incoming
// dependency covers only color-attachment output, not the depth/stencil
// writes of earlier passes.
vk::UniqueRenderPass BuildWithColorOnlyIncomingDependency(
    const vk::Device& device,
    const RenderPassBuilderVK& builder) {
  if (!builder.GetColor0().has_value() ||
      !builder.GetColor0Resolve().has_value() ||
      !builder.GetDepthStencil().has_value()) {
    return {};
  }
  std::array<vk::AttachmentDescription, 3> attachments = {
      builder.GetColor0().value(),
      builder.GetColor0Resolve().value(),
      builder.GetDepthStencil().value(),
  };
  vk::AttachmentReference color_ref;
  color_ref.attachment = 0u;
  color_ref.layout = vk::ImageLayout::eGeneral;
  vk::AttachmentReference resolve_ref;
  resolve_ref.attachment = 1u;
  resolve_ref.layout = vk::ImageLayout::eGeneral;
  vk::AttachmentReference depth_stencil_ref;
  depth_stencil_ref.attachment = 2u;
  depth_stencil_ref.layout = vk::ImageLayout::eDepthStencilAttachmentOptimal;

  vk::SubpassDescription subpass;
  subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
  subpass.setPInputAttachments(&color_ref);
  subpass.setInputAttachmentCount(1u);
  subpass.setPColorAttachments(&color_ref);
  subpass.setColorAttachmentCount(1u);
  subpass.setPResolveAttachments(&resolve_ref);
  subpass.setPDepthStencilAttachment(&depth_stencil_ref);

  std::array<vk::SubpassDependency, 3> deps;
  deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  deps[0].dstSubpass = 0u;
  deps[0].srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput |
                         vk::PipelineStageFlagBits::eFragmentShader;
  deps[0].srcAccessMask = vk::AccessFlagBits::eShaderRead |
                          vk::AccessFlagBits::eColorAttachmentWrite;
  deps[0].dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
  deps[0].dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
  deps[0].dependencyFlags = vk::DependencyFlagBits::eByRegion;

  deps[1].srcSubpass = 0u;
  deps[1].dstSubpass = 0u;
  deps[1].srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
  deps[1].srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
  deps[1].dstStageMask = vk::PipelineStageFlagBits::eFragmentShader;
  deps[1].dstAccessMask = vk::AccessFlagBits::eInputAttachmentRead;
  deps[1].dependencyFlags = vk::DependencyFlagBits::eByRegion;

  deps[2].srcSubpass = 0u;
  deps[2].dstSubpass = VK_SUBPASS_EXTERNAL;
  deps[2].srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
  deps[2].srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
  deps[2].dstStageMask = vk::PipelineStageFlagBits::eFragmentShader;
  deps[2].dstAccessMask = vk::AccessFlagBits::eShaderRead;
  deps[2].dependencyFlags = vk::DependencyFlagBits::eByRegion;

  vk::RenderPassCreateInfo info;
  info.setPAttachments(attachments.data());
  info.setAttachmentCount(static_cast<uint32_t>(attachments.size()));
  info.setSubpasses(subpass);
  info.setPDependencies(deps.data());
  info.setDependencyCount(static_cast<uint32_t>(deps.size()));
  auto [result, pass] = device.createRenderPassUnique(info);
  if (result != vk::Result::eSuccess) {
    return {};
  }
  return std::move(pass);
}

}  // namespace

// Two same-sized root targets in one frame share the pool's one attachment
// set (patch 46a) and render one after the other on the graphics queue, as
// two views do. Synchronization validation must report no hazard on the
// shared MSAA color or depth/stencil attachments.
TEST_P(RendererTest, TwoSameSizeRootTargetsShareTransientsWithoutHazard) {
  if (GetBackend() != PlaygroundBackend::kVulkan) {
    GTEST_SKIP() << "Test only applies to Vulkan";
  }
  if (!SyncValidationRequested()) {
    GTEST_SKIP() << kSyncValidationHint;
  }
  std::shared_ptr<Context> context = GetContext();
  ASSERT_TRUE(context);
  std::shared_ptr<TransientsPoolVK> pool =
      ContextVK::Cast(*context).GetSwapchainTransientsPool();
  ASSERT_TRUE(pool);
  const TextureDescriptor desc = RootTargetDescriptor(*context);

  std::shared_ptr<SwapchainTransientsVK> shared_transients;
  std::vector<std::shared_ptr<Texture>> resolves;
  std::vector<std::unique_ptr<SurfaceVK>> surfaces;
  for (int view = 0; view < 2; view++) {
    std::shared_ptr<SwapchainTransientsVK> transients =
        pool->Acquire(desc, /*enable_msaa=*/true);
    ASSERT_TRUE(transients);
    if (shared_transients) {
      // The second view's acquisition shares the first view's set.
      EXPECT_EQ(transients, shared_transients);
    } else {
      shared_transients = transients;
    }
    std::shared_ptr<Texture> resolve =
        context->GetResourceAllocator()->CreateTexture(desc);
    ASSERT_TRUE(resolve);
    auto source = std::const_pointer_cast<TextureSourceVK>(
        TextureVK::Cast(*resolve).GetTextureSource());
    auto surface =
        SurfaceVK::WrapSwapchainImage(transients, source, [] { return true; });
    ASSERT_TRUE(surface);
    ASSERT_TRUE(surface->GetRenderTarget().GetDepthAttachment().has_value());
    resolves.push_back(std::move(resolve));
    surfaces.push_back(std::move(surface));
  }

  ScopedSyncHazardCounter counter;
  // Both root passes are recorded back to back in one submission, the
  // tightest ordering two views of one raster frame can have.
  auto buffer = context->CreateCommandBuffer();
  ASSERT_TRUE(buffer);
  for (const auto& surface : surfaces) {
    auto pass = buffer->CreateRenderPass(surface->GetRenderTarget());
    ASSERT_TRUE(pass);
    ASSERT_TRUE(pass->EncodeCommands());
  }
  ASSERT_TRUE(SubmitAndWait(context, buffer));
  EXPECT_EQ(counter.hazards(), 0u);
}

// The negative control: the same two passes over one shared attachment set,
// recorded directly, once with the render pass RenderPassBuilderVK builds now
// and once with the render pass it built before patch 46a (color-only
// incoming dependency). Synchronization validation must report a hazard on
// the shared depth/stencil attachment for the old pass and none for the new
// one. This shows the setup above can see the hazard the widened dependency
// closes.
TEST_P(RendererTest, SharedDepthStencilNeedsTheWidenedIncomingDependency) {
  if (GetBackend() != PlaygroundBackend::kVulkan) {
    GTEST_SKIP() << "Test only applies to Vulkan";
  }
  if (!SyncValidationRequested()) {
    GTEST_SKIP() << kSyncValidationHint;
  }
  std::shared_ptr<Context> context = GetContext();
  ASSERT_TRUE(context);
  const ContextVK& context_vk = ContextVK::Cast(*context);
  const vk::Device& device = context_vk.GetDevice();
  std::shared_ptr<TransientsPoolVK> pool =
      context_vk.GetSwapchainTransientsPool();
  ASSERT_TRUE(pool);
  const TextureDescriptor desc = RootTargetDescriptor(*context);
  std::shared_ptr<SwapchainTransientsVK> transients =
      pool->Acquire(desc, /*enable_msaa=*/true);
  ASSERT_TRUE(transients);
  const std::shared_ptr<Texture> msaa = transients->GetMSAATexture();
  const std::shared_ptr<Texture> depth_stencil =
      transients->GetDepthStencilTexture();
  ASSERT_TRUE(msaa);
  ASSERT_TRUE(depth_stencil);
  std::array<std::shared_ptr<Texture>, 2> resolves = {
      context->GetResourceAllocator()->CreateTexture(desc),
      context->GetResourceAllocator()->CreateTexture(desc),
  };
  ASSERT_TRUE(resolves[0]);
  ASSERT_TRUE(resolves[1]);

  RenderPassBuilderVK builder;
  builder.SetColorAttachment(0u, desc.format, SampleCount::kCount4,
                             LoadAction::kClear,
                             StoreAction::kMultisampleResolve);
  builder.SetDepthStencilAttachment(
      depth_stencil->GetTextureDescriptor().format, SampleCount::kCount4,
      LoadAction::kClear, StoreAction::kDontCare);
  vk::UniqueRenderPass widened = builder.Build(device);
  vk::UniqueRenderPass color_only =
      BuildWithColorOnlyIncomingDependency(device, builder);
  ASSERT_TRUE(widened);
  ASSERT_TRUE(color_only);

  const auto count_hazards = [&](vk::RenderPass render_pass) -> size_t {
    std::vector<vk::UniqueFramebuffer> framebuffers;
    for (const auto& resolve : resolves) {
      std::array<vk::ImageView, 3> views = {
          TextureVK::Cast(*msaa).GetRenderTargetView(),
          TextureVK::Cast(*resolve).GetRenderTargetView(),
          TextureVK::Cast(*depth_stencil).GetRenderTargetView(),
      };
      vk::FramebufferCreateInfo info;
      info.renderPass = render_pass;
      info.setPAttachments(views.data());
      info.setAttachmentCount(static_cast<uint32_t>(views.size()));
      info.width = static_cast<uint32_t>(desc.size.width);
      info.height = static_cast<uint32_t>(desc.size.height);
      info.layers = 1u;
      auto [result, framebuffer] = device.createFramebufferUnique(info);
      if (result != vk::Result::eSuccess) {
        ADD_FAILURE() << "Could not create framebuffer: "
                      << vk::to_string(result);
        return 0u;
      }
      framebuffers.push_back(std::move(framebuffer));
    }

    std::array<vk::ClearValue, 3> clears;
    clears[0].color = vk::ClearColorValue(std::array<float, 4>{0, 0, 0, 0});
    clears[1].color = vk::ClearColorValue(std::array<float, 4>{0, 0, 0, 0});
    clears[2].depthStencil = vk::ClearDepthStencilValue(1.0f, 0u);

    ScopedSyncHazardCounter counter;
    auto buffer = context->CreateCommandBuffer();
    if (!buffer) {
      ADD_FAILURE() << "Could not create a command buffer.";
      return 0u;
    }
    vk::CommandBuffer command_buffer =
        CommandBufferVK::Cast(*buffer).GetCommandBuffer();
    for (const auto& framebuffer : framebuffers) {
      vk::RenderPassBeginInfo begin;
      begin.renderPass = render_pass;
      begin.framebuffer = framebuffer.get();
      begin.renderArea.offset = vk::Offset2D(0, 0);
      begin.renderArea.extent =
          vk::Extent2D(static_cast<uint32_t>(desc.size.width),
                       static_cast<uint32_t>(desc.size.height));
      begin.setPClearValues(clears.data());
      begin.setClearValueCount(static_cast<uint32_t>(clears.size()));
      command_buffer.beginRenderPass(begin, vk::SubpassContents::eInline);
      command_buffer.endRenderPass();
    }
    if (!SubmitAndWait(context, buffer)) {
      ADD_FAILURE() << "Could not submit the command buffer.";
    }
    return counter.hazards();
  };

  // The current pass first: its attachments start untouched.
  EXPECT_EQ(count_hazards(widened.get()), 0u);
  EXPECT_GT(count_hazards(color_only.get()), 0u);
}

}  // namespace testing
}  // namespace impeller
