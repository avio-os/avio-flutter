// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>

#include "flutter/testing/testing.h"
#include "gtest/gtest.h"
#include "impeller/renderer/backend/vulkan/command_buffer_vk.h"
#include "impeller/renderer/backend/vulkan/context_vk.h"
#include "impeller/renderer/backend/vulkan/test/mock_vulkan.h"
#include "impeller/renderer/backend/vulkan/texture_vk.h"
#include "impeller/renderer/backend/vulkan/timeline_completion_vk.h"

namespace impeller {
namespace testing {

class AcquiringSourceVK final : public TextureSourceVK {
 public:
  AcquiringSourceVK(const std::shared_ptr<ContextVK>& context,
                    TextureDescriptor descriptor)
      : TextureSourceVK(descriptor) {
    auto result = context->GetDevice().createSemaphoreUnique({});
    FML_CHECK(result.result == vk::Result::eSuccess);
    acquire_ = std::move(result.value);
  }
  vk::Image GetImage() const override { return {}; }
  vk::ImageView GetImageView() const override { return {}; }
  vk::ImageView GetRenderTargetView(uint32_t, uint32_t) const override {
    return {};
  }
  bool IsSwapchainImage() const override { return false; }
  std::optional<WaitSemaphore> ConsumeAcquireSemaphore() const override {
    if (!acquire_) {
      return std::nullopt;
    }
    consumes_++;
    WaitSemaphore wait;
    wait.semaphore = std::move(acquire_);
    wait.wait_stage = vk::PipelineStageFlagBits::eFragmentShader;
    return wait;
  }
  uint32_t Consumes() const { return consumes_; }

 private:
  mutable vk::UniqueSemaphore acquire_;
  mutable uint32_t consumes_ = 0;
};

TEST(CommandQueueVKTest, ReturnedAcquireRetainsExactHandleWithoutSourceCycle) {
  auto context = MockVulkanContextBuilder().Build();
  auto source =
      std::make_shared<AcquiringSourceVK>(context, TextureDescriptor{});
  auto initial = source->TakeAcquireSemaphoreForSubmit();
  ASSERT_TRUE(initial);
  const auto handle = initial->semaphore.get();
  initial->source = source;
  source->ReturnAcquireSemaphoreFromFailedSubmit(std::move(*initial));
  auto retry = source->TakeAcquireSemaphoreForSubmit();
  ASSERT_TRUE(retry);
  EXPECT_EQ(retry->semaphore.get(), handle);
  EXPECT_EQ(source->Consumes(), 1u);
  EXPECT_FALSE(source->TakeAcquireSemaphoreForSubmit());
  retry.reset();
  std::weak_ptr<AcquiringSourceVK> weak = source;
  source.reset();
  EXPECT_TRUE(weak.expired());
}

TEST(CommandQueueVKTest, ReturnedAcquireClosesOnceWithItsLastOwner) {
  auto context = MockVulkanContextBuilder().Build();
  const auto calls = GetMockVulkanFunctions(context->GetDevice());
  const auto before =
      std::count(calls->begin(), calls->end(), "vkDestroySemaphore");
  auto source =
      std::make_shared<AcquiringSourceVK>(context, TextureDescriptor{});
  auto wait = source->TakeAcquireSemaphoreForSubmit();
  ASSERT_TRUE(wait);
  wait->source = source;
  source->ReturnAcquireSemaphoreFromFailedSubmit(std::move(*wait));
  std::weak_ptr<AcquiringSourceVK> weak = source;
  source.reset();
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(std::count(calls->begin(), calls->end(), "vkDestroySemaphore"),
            before + 1);
}

TEST(CommandQueueVKTest, PendingAcquireKeepsClosedSourceAliveUntilCompletion) {
  auto context = MockVulkanContextBuilder().Build();
  auto source =
      std::make_shared<AcquiringSourceVK>(context, TextureDescriptor{});
  auto wait = source->TakeAcquireSemaphoreForSubmit();
  ASSERT_TRUE(wait);
  wait->source = source;
  std::weak_ptr<AcquiringSourceVK> weak = source;
  source.reset();
  EXPECT_FALSE(weak.expired());
  wait.reset();
  EXPECT_TRUE(weak.expired());
}

TEST(CommandQueueVKTest, AbandonedRecordingDoesNotConsumeProducerDependency) {
  auto context = MockVulkanContextBuilder().Build();
  auto source =
      std::make_shared<AcquiringSourceVK>(context, TextureDescriptor{});
  {
    auto abandoned = context->CreateCommandBuffer();
    ASSERT_TRUE(CommandBufferVK::Cast(*abandoned).Track(source));
    EXPECT_EQ(source->Consumes(), 0u);
  }
  auto reader = context->CreateCommandBuffer();
  ASSERT_TRUE(CommandBufferVK::Cast(*reader).Track(source));
  EXPECT_TRUE(context->GetCommandQueue()->Submit({reader}).ok());
  EXPECT_EQ(source->Consumes(), 1u);
  const auto waits = GetMockVulkanQueueSubmitWaitCounts();
  ASSERT_FALSE(waits.empty());
  ASSERT_EQ(waits.back().size(), 2u);
  EXPECT_EQ(waits.back()[0], 1u);
}

TEST(CommandQueueVKTest, FailedSubmitReturnsExactWaitToNextSubmittedReader) {
  uint32_t attempts = 0;
  auto context =
      MockVulkanContextBuilder()
          .SetQueueSubmitCallback([&]() {
            return attempts++ == 0 ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;
          })
          .Build();
  auto source =
      std::make_shared<AcquiringSourceVK>(context, TextureDescriptor{});
  auto failed = context->CreateCommandBuffer();
  auto next = context->CreateCommandBuffer();
  ASSERT_TRUE(CommandBufferVK::Cast(*failed).Track(source));
  ASSERT_TRUE(CommandBufferVK::Cast(*next).Track(source));
  EXPECT_FALSE(context->GetCommandQueue()->Submit({failed}).ok());
  EXPECT_EQ(source->Consumes(), 1u);
  EXPECT_TRUE(context->GetCommandQueue()->Submit({next}).ok());
  // Restoration retains the exact semaphore: no second producer consumption.
  EXPECT_EQ(source->Consumes(), 1u);
  const auto waits = GetMockVulkanQueueSubmitWaitCounts();
  ASSERT_EQ(waits.size(), 2u);
  EXPECT_EQ(waits[0][0], 1u);
  EXPECT_EQ(waits[1][0], 1u);
}

TEST(CommandQueueVKTest, TwoRecordedReadersConsumeOnlyFirstSubmittedWait) {
  auto context = MockVulkanContextBuilder().Build();
  auto source =
      std::make_shared<AcquiringSourceVK>(context, TextureDescriptor{});
  auto earlier_recorded = context->CreateCommandBuffer();
  auto later_recorded = context->CreateCommandBuffer();
  ASSERT_TRUE(CommandBufferVK::Cast(*earlier_recorded).Track(source));
  ASSERT_TRUE(CommandBufferVK::Cast(*later_recorded).Track(source));
  EXPECT_EQ(source->Consumes(), 0u);
  EXPECT_TRUE(context->GetCommandQueue()->Submit({later_recorded}).ok());
  EXPECT_TRUE(context->GetCommandQueue()->Submit({earlier_recorded}).ok());
  const auto waits = GetMockVulkanQueueSubmitWaitCounts();
  ASSERT_EQ(waits.size(), 2u);
  EXPECT_EQ(waits[0][0], 1u);
  EXPECT_EQ(waits[1][0], 0u);
  EXPECT_EQ(source->Consumes(), 1u);
}

TEST(CommandQueueVKTest,
     ClosedSubmissionEpochLeavesAcquireDependencyUntouched) {
  auto context = MockVulkanContextBuilder().Build();
  auto source =
      std::make_shared<AcquiringSourceVK>(context, TextureDescriptor{});
  auto reader = context->CreateCommandBuffer();
  ASSERT_TRUE(CommandBufferVK::Cast(*reader).Track(source));
  context->GetTimelineCompletion()->Terminate();
  EXPECT_FALSE(context->GetCommandQueue()->Submit({reader}).ok());
  EXPECT_EQ(source->Consumes(), 0u);
  EXPECT_TRUE(GetMockVulkanQueueSubmitWaitCounts().empty());
}

TEST(CommandQueueVKTest, QueueSubmit) {
  const auto context = MockVulkanContextBuilder().Build();
  auto buffer = context->CreateCommandBuffer();
  auto status = context->GetCommandQueue()->Submit({buffer});
  EXPECT_TRUE(status.ok());

  const auto called = GetMockVulkanFunctions(context->GetDevice());
  EXPECT_NE(std::find(called->begin(), called->end(), "vkQueueSubmit"),
            called->end());
}

TEST(CommandQueueVKTest, SubmitAfterTimelineCompletionTerminated) {
  const auto context = MockVulkanContextBuilder().Build();
  auto buffer = context->CreateCommandBuffer();
  context->GetTimelineCompletion()->Terminate();
  auto status = context->GetCommandQueue()->Submit({buffer});
  EXPECT_EQ(status.code(), fml::StatusCode::kCancelled);

  // The command buffer should not be submitted to the Vulkan queue if the
  // completion tracker has been terminated.
  const auto called = GetMockVulkanFunctions(context->GetDevice());
  EXPECT_EQ(std::find(called->begin(), called->end(), "vkQueueSubmit"),
            called->end());
}

}  // namespace testing
}  // namespace impeller
