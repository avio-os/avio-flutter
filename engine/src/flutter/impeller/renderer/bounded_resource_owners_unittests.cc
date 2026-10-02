// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/bounded_resource_owners.h"

#include "gtest/gtest.h"
#include "impeller/renderer/command_buffer.h"
#include "impeller/renderer/command_queue.h"
#include "impeller/renderer/render_target.h"

namespace impeller {
namespace testing {
namespace {

// CPU custody fixture. It exercises the real command-buffer owner handoff;
// no Vulkan device, GPU completion or driver allocation is simulated as valid.
class OwnerCommandBuffer final : public CommandBuffer {
 public:
  OwnerCommandBuffer() : CommandBuffer({}) {}
  bool IsValid() const override { return HasValidResourceOwners(); }
  void SetLabel(std::string_view) const override {}
  int submit_calls = 0;
  int pass_creation_calls = 0;

 private:
  std::shared_ptr<RenderPass> OnCreateRenderPass(RenderTarget) override {
    pass_creation_calls++;
    return nullptr;
  }
  std::shared_ptr<BlitPass> OnCreateBlitPass() override {
    pass_creation_calls++;
    return nullptr;
  }
  std::shared_ptr<ComputePass> OnCreateComputePass() override {
    pass_creation_calls++;
    return nullptr;
  }
  bool OnSubmitCommands(bool, CompletionCallback) override {
    submit_calls++;
    return false;
  }
  void OnWaitUntilCompleted() override {}
  void OnWaitUntilScheduled() override {}
};

TEST(BoundedResourceOwnersTest, CacheRemovalKeepsExactSubmittedOwner) {
  auto cached = std::make_shared<int>(17);
  std::weak_ptr<int> witness = cached;
  BoundedResourceOwners recording;
  ASSERT_TRUE(recording.Retain(cached));
  cached.reset();
  EXPECT_FALSE(witness.expired());
  auto submitted = std::move(recording);
  EXPECT_EQ(recording.size(), 0u);
  EXPECT_EQ(submitted.size(), 1u);
  EXPECT_FALSE(witness.expired());
  // An unobservable completion leaves this exact native submission alive.
  EXPECT_FALSE(witness.expired());
  submitted.Clear();
  EXPECT_TRUE(witness.expired());
}

TEST(BoundedResourceOwnersTest, WrapperDropCannotReleaseTransferredOwners) {
  auto command = std::make_shared<OwnerCommandBuffer>();
  auto region = std::make_shared<int>(23);
  std::weak_ptr<int> witness = region;
  ASSERT_TRUE(command->TryRetainResource(region));
  auto native_submission = command->TakeResourceOwners();
  region.reset();
  command.reset();
  EXPECT_FALSE(witness.expired());
  native_submission.Clear();
  EXPECT_TRUE(witness.expired());
}

TEST(BoundedResourceOwnersTest, DuplicateOwnersUseOneBoundedEntry) {
  BoundedResourceOwners owners;
  auto pipeline = std::make_shared<int>(17);
  for (int draw = 0; draw < 8192; draw++) {
    ASSERT_TRUE(owners.Retain(pipeline));
  }
  EXPECT_EQ(owners.size(), 1u);
  EXPECT_TRUE(owners.IsValid());
}

TEST(BoundedResourceOwnersTest, CapacityFailurePoisonsBeforeSubmission) {
  auto command = std::make_shared<OwnerCommandBuffer>();
  std::array<std::shared_ptr<int>, BoundedResourceOwners::kCapacity> sources;
  for (auto& source : sources) {
    source = std::make_shared<int>(17);
    ASSERT_TRUE(command->TryRetainResource(source));
  }
  EXPECT_FALSE(command->TryRetainResource(std::make_shared<int>(99)));
  EXPECT_FALSE(command->IsValid());
  EXPECT_FALSE(command->CreateRenderPass(RenderTarget{}));
  EXPECT_FALSE(command->CreateBlitPass());
  EXPECT_FALSE(command->CreateComputePass());
  EXPECT_EQ(command->pass_creation_calls, 0);
  int errors = 0;
  CommandQueue queue;
  EXPECT_FALSE(queue
                   .Submit({command},
                           [&](CommandBuffer::Status status) {
                             EXPECT_EQ(status, CommandBuffer::Status::kError);
                             errors++;
                           })
                   .ok());
  EXPECT_EQ(command->submit_calls, 0);
  EXPECT_EQ(errors, 1);
  EXPECT_FALSE(command->TryRetainResource(sources[0]));
  auto refused = command->TakeResourceOwners();
  EXPECT_FALSE(refused.IsValid());
  EXPECT_EQ(refused.size(), sources.size());
  EXPECT_FALSE(command->IsValid());
  EXPECT_FALSE(command->TryRetainResource(sources[0]));
}

TEST(BoundedResourceOwnersTest, AbortReleasesOnlyItsOwnUnsubmittedBatch) {
  auto first = std::make_shared<int>(17);
  auto second = std::make_shared<int>(23);
  std::weak_ptr<int> in_flight = first;
  std::weak_ptr<int> aborted = second;
  BoundedResourceOwners submitted;
  ASSERT_TRUE(submitted.Retain(first));
  {
    BoundedResourceOwners recording;
    ASSERT_TRUE(recording.Retain(second));
    first.reset();
    second.reset();
  }
  EXPECT_TRUE(aborted.expired());
  EXPECT_FALSE(in_flight.expired());
  submitted.Clear();
  EXPECT_TRUE(in_flight.expired());
}

}  // namespace
}  // namespace testing
}  // namespace impeller
