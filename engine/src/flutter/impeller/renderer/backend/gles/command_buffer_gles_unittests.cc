// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

#include "flutter/fml/mapping.h"
#include "flutter/testing/testing.h"  // IWYU pragma: keep
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "impeller/core/formats.h"
#include "impeller/renderer/backend/gles/context_gles.h"
#include "impeller/renderer/backend/gles/reactor_gles.h"
#include "impeller/renderer/backend/gles/test/mock_gles.h"
#include "impeller/renderer/backend/gles/texture_gles.h"
#include "impeller/renderer/blit_pass.h"
#include "impeller/renderer/command_queue.h"
#include "impeller/renderer/context.h"
#include "impeller/renderer/snapshot.h"

namespace impeller {
namespace testing {
namespace {

using ::testing::_;
using ::testing::NiceMock;

class ToggleWorker final : public ReactorGLES::Worker {
 public:
  explicit ToggleWorker(bool allowed) : allowed_(allowed) {}

  bool CanReactorReactOnCurrentThreadNow(
      const ReactorGLES& reactor) const override {
    return allowed_.load();
  }

  void SetAllowed(bool allowed) { allowed_.store(allowed); }

 private:
  std::atomic<bool> allowed_;
};

std::shared_ptr<ContextGLES> CreateFakeGLESContext() {
  auto gl = std::make_unique<ProcTableGLES>(kMockResolverGLES);
  return ContextGLES::Create(Flags{}, std::move(gl),
                             std::vector<std::shared_ptr<fml::Mapping>>{},
                             /*enable_gpu_tracing=*/false);
}

}  // namespace

TEST(CommandBufferGLES,
     SubmitWithoutCurrentContextIsAcceptedAndCompletesAfterReaction) {
  auto mock_gles = MockGLES::Init();
  auto context = CreateFakeGLESContext();
  auto context_base = std::static_pointer_cast<Context>(context);
  auto worker = std::make_shared<ToggleWorker>(false);
  context->AddReactorWorker(worker);

  auto command_buffer = context_base->CreateCommandBuffer();
  ASSERT_TRUE(command_buffer);

  bool callback_called = false;
  CommandBuffer::Status callback_status = CommandBuffer::Status::kError;
  auto status = context_base->GetCommandQueue()->Submit(
      {command_buffer}, [&](CommandBuffer::Status status) {
        callback_called = true;
        callback_status = status;
      });

  EXPECT_TRUE(status.ok());
  EXPECT_FALSE(callback_called);

  worker->SetAllowed(true);
  EXPECT_TRUE(context->GetReactor()->React());
  EXPECT_TRUE(callback_called);
  EXPECT_EQ(callback_status, CommandBuffer::Status::kCompleted);
}

TEST(CommandBufferGLES, SubmitWithoutCallbackIsAcceptedWithoutCurrentContext) {
  auto mock_gles = MockGLES::Init();
  auto context = CreateFakeGLESContext();
  auto context_base = std::static_pointer_cast<Context>(context);
  auto worker = std::make_shared<ToggleWorker>(false);
  context->AddReactorWorker(worker);

  auto command_buffer = context_base->CreateCommandBuffer();
  ASSERT_TRUE(command_buffer);

  EXPECT_TRUE(context_base->GetCommandQueue()->Submit({command_buffer}).ok());
}

TEST(CommandBufferGLES, DeferredSubmitCallbackErrorsIfReactorIsDestroyed) {
  bool callback_called = false;
  CommandBuffer::Status callback_status = CommandBuffer::Status::kCompleted;

  {
    auto mock_gles = MockGLES::Init();
    auto context = CreateFakeGLESContext();
    auto context_base = std::static_pointer_cast<Context>(context);
    auto worker = std::make_shared<ToggleWorker>(false);
    context->AddReactorWorker(worker);

    auto command_buffer = context_base->CreateCommandBuffer();
    ASSERT_TRUE(command_buffer);
    auto status = context_base->GetCommandQueue()->Submit(
        {command_buffer}, [&](CommandBuffer::Status status) {
          callback_called = true;
          callback_status = status;
        });

    EXPECT_TRUE(status.ok());
    EXPECT_FALSE(callback_called);
  }

  EXPECT_TRUE(callback_called);
  EXPECT_EQ(callback_status, CommandBuffer::Status::kError);
}

TEST(CommandBufferGLES, DeferredSubmissionRetainsOwnersUntilReaction) {
  auto mock_gles = MockGLES::Init();
  auto context = CreateFakeGLESContext();
  auto context_base = std::static_pointer_cast<Context>(context);
  auto worker = std::make_shared<ToggleWorker>(false);
  context->AddReactorWorker(worker);
  auto command = context_base->CreateCommandBuffer();
  auto owner = std::make_shared<int>(17);
  std::weak_ptr<int> witness = owner;
  ASSERT_TRUE(command->TryRetainResource(owner));
  ASSERT_TRUE(context_base->GetCommandQueue()->Submit({command}).ok());
  owner.reset();
  command.reset();
  EXPECT_FALSE(witness.expired());
  worker->SetAllowed(true);
  ASSERT_TRUE(context->GetReactor()->React());
  EXPECT_TRUE(witness.expired());
}

TEST(CommandBufferGLES, BorrowedTargetCollectionFollowsQueuedSourceReferences) {
  auto implementation = std::make_unique<::testing::NiceMock<MockGLESImpl>>();
  auto& gl = *implementation;
  auto mock = MockGLES::Init(std::move(implementation));
  auto context = CreateFakeGLESContext();
  auto worker = std::make_shared<ToggleWorker>(false);
  context->AddReactorWorker(worker);
  TextureDescriptor descriptor;
  descriptor.format = PixelFormat::kR8G8B8A8UNormInt;
  descriptor.size = {10, 10};
  descriptor.usage = TextureUsage::kRenderTarget;
  EXPECT_CALL(gl, GenTextures(_, _)).Times(0);
  EXPECT_CALL(gl, DeleteTextures(_, _)).Times(0);
  auto source = TextureGLES::WrapFBO(context->GetReactor(), descriptor, 91);
  ASSERT_TRUE(source);
  std::vector<int> order;
  ASSERT_TRUE(
      source->AdoptExternalResourceCallback([&] { order.push_back(2); }));
  EXPECT_FALSE(source->AdoptExternalResourceCallback([] {}));
  ASSERT_TRUE(context->GetReactor()->AddOperation(
      [source, &order](const ReactorGLES&) {
        EXPECT_EQ(source->GetFBO(), 91u);
        order.push_back(1);
      },
      true));
  source.reset();
  EXPECT_TRUE(order.empty());
  worker->SetAllowed(true);
  ASSERT_TRUE(context->GetReactor()->React());
  EXPECT_EQ(order, (std::vector<int>{1, 2}));
}

TEST(CommandBufferGLES, ShutdownReleasesQueuedBorrowedSourcesWithoutExecution) {
  auto mock = MockGLES::Init();
  auto context = CreateFakeGLESContext();
  auto base = std::static_pointer_cast<Context>(context);
  auto worker = std::make_shared<ToggleWorker>(false);
  context->AddReactorWorker(worker);
  TextureDescriptor descriptor;
  descriptor.format = PixelFormat::kR8G8B8A8UNormInt;
  descriptor.size = {10, 10};
  descriptor.usage = TextureUsage::kRenderTarget;
  auto source = TextureGLES::WrapFBO(context->GetReactor(), descriptor, 91);
  ASSERT_TRUE(source);
  int collections = 0;
  bool executed = false;
  ASSERT_TRUE(source->AdoptExternalResourceCallback([&] {
    collections++;
    EXPECT_FALSE(
        context->GetReactor()->AddOperation([](const ReactorGLES&) {}, true));
  }));
  ASSERT_TRUE(context->GetReactor()->AddOperation(
      [source, &executed](const ReactorGLES&) { executed = true; }, true));
  auto command = base->CreateCommandBuffer();
  int completions = 0;
  EXPECT_TRUE(base->GetCommandQueue()
                  ->Submit({command},
                           [&](CommandBuffer::Status status) {
                             completions++;
                             EXPECT_EQ(status, CommandBuffer::Status::kError);
                           })
                  .ok());
  source.reset();
  command.reset();
  EXPECT_EQ(collections, 0);
  base->Shutdown();
  EXPECT_FALSE(executed);
  EXPECT_EQ(collections, 1);
  EXPECT_EQ(completions, 1);
  EXPECT_FALSE(base->IsValid());
  worker->SetAllowed(true);
  EXPECT_FALSE(context->GetReactor()->React());
  base->Shutdown();
  EXPECT_EQ(collections, 1);
  EXPECT_EQ(completions, 1);
}

TEST(CommandBufferGLES, DiscardedSubmissionHasNoReactorOwnerCycle) {
  auto mock_gles = MockGLES::Init();
  auto owner = std::make_shared<int>(23);
  std::weak_ptr<int> witness = owner;
  std::weak_ptr<ReactorGLES> reactor;
  {
    auto context = CreateFakeGLESContext();
    auto context_base = std::static_pointer_cast<Context>(context);
    auto worker = std::make_shared<ToggleWorker>(false);
    context->AddReactorWorker(worker);
    reactor = context->GetReactor();
    auto command = context_base->CreateCommandBuffer();
    ASSERT_TRUE(command->TryRetainResource(owner));
    ASSERT_TRUE(context_base->GetCommandQueue()->Submit({command}).ok());
    owner.reset();
    command.reset();
    EXPECT_FALSE(witness.expired());
  }
  EXPECT_TRUE(reactor.expired());
  EXPECT_TRUE(witness.expired());
}

TEST(CommandBufferGLES, DeferredSubmitCompletesAfterPreviouslyQueuedWork) {
  auto mock_gles = MockGLES::Init();
  auto context = CreateFakeGLESContext();
  auto context_base = std::static_pointer_cast<Context>(context);
  auto worker = std::make_shared<ToggleWorker>(false);
  context->AddReactorWorker(worker);

  std::vector<int> order;
  ASSERT_TRUE(context->GetReactor()->AddOperation(
      [&](const ReactorGLES& reactor) { order.push_back(1); },
      /*defer=*/true));

  auto command_buffer = context_base->CreateCommandBuffer();
  ASSERT_TRUE(command_buffer);
  auto status = context_base->GetCommandQueue()->Submit(
      {command_buffer},
      [&](CommandBuffer::Status status) { order.push_back(2); });

  EXPECT_TRUE(status.ok());
  EXPECT_TRUE(order.empty());

  worker->SetAllowed(true);
  EXPECT_TRUE(context->GetReactor()->React());
  EXPECT_THAT(order, ::testing::ElementsAre(1, 2));
}

TEST(CommandBufferGLES, LaterCurrentSubmitDrainsPreviouslyDeferredWork) {
  auto mock_gles = MockGLES::Init();
  auto context = CreateFakeGLESContext();
  auto context_base = std::static_pointer_cast<Context>(context);
  auto worker = std::make_shared<ToggleWorker>(false);
  context->AddReactorWorker(worker);

  std::vector<int> order;
  ASSERT_TRUE(context->GetReactor()->AddOperation(
      [&](const ReactorGLES& reactor) { order.push_back(1); },
      /*defer=*/true));

  auto deferred_command_buffer = context_base->CreateCommandBuffer();
  ASSERT_TRUE(deferred_command_buffer);
  EXPECT_TRUE(
      context_base->GetCommandQueue()->Submit({deferred_command_buffer}).ok());
  EXPECT_TRUE(order.empty());

  worker->SetAllowed(true);
  auto current_command_buffer = context_base->CreateCommandBuffer();
  ASSERT_TRUE(current_command_buffer);
  EXPECT_TRUE(
      context_base->GetCommandQueue()
          ->Submit({current_command_buffer},
                   [&](CommandBuffer::Status status) { order.push_back(2); })
          .ok());
  EXPECT_THAT(order, ::testing::ElementsAre(1, 2));
}

TEST(CommandBufferGLES, BufferToTextureBlitCanBeSubmittedBeforeContextCurrent) {
  auto mock_gles_impl = std::make_unique<NiceMock<MockGLESImpl>>();
  auto& mock_gles_impl_ref = *mock_gles_impl;
  auto mock_gles = MockGLES::Init(std::move(mock_gles_impl));
  auto context = CreateFakeGLESContext();
  auto context_base = std::static_pointer_cast<Context>(context);
  auto worker = std::make_shared<ToggleWorker>(false);
  context->AddReactorWorker(worker);

  constexpr GLuint kTextureHandle = 1234;
  bool texture_generated = false;
  bool texture_uploaded = false;
  EXPECT_CALL(mock_gles_impl_ref, GenTextures(1, _))
      .WillOnce([&](GLsizei size, GLuint* textures) {
        texture_generated = true;
        textures[0] = kTextureHandle;
      });
  EXPECT_CALL(mock_gles_impl_ref, BindTexture(GL_TEXTURE_2D, kTextureHandle))
      .Times(::testing::AtLeast(1));
  EXPECT_CALL(mock_gles_impl_ref,
              TexImage2D(GL_TEXTURE_2D, 0, _, 2, 2, 0, _, _, nullptr))
      .Times(1);
  EXPECT_CALL(mock_gles_impl_ref,
              TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 2, _, _, _))
      .WillOnce([&](GLenum target, GLint level, GLint xoffset, GLint yoffset,
                    GLsizei width, GLsizei height, GLenum format, GLenum type,
                    const void* pixels) { texture_uploaded = true; });

  TextureDescriptor texture_descriptor;
  texture_descriptor.size = {2, 2};
  texture_descriptor.format = PixelFormat::kR8G8B8A8UNormInt;
  auto texture = context_base->GetResourceAllocator()->CreateTexture(
      texture_descriptor, /*threadsafe=*/true);
  ASSERT_TRUE(texture);

  const uint8_t pixels[16] = {};
  auto buffer =
      context_base->GetResourceAllocator()->CreateBufferWithCopy(pixels, 16);
  ASSERT_TRUE(buffer);

  auto command_buffer = context_base->CreateCommandBuffer();
  ASSERT_TRUE(command_buffer);
  auto blit_pass = command_buffer->CreateBlitPass();
  ASSERT_TRUE(blit_pass);

  EXPECT_TRUE(blit_pass->AddCopy(BufferView(buffer, Range(0, 16)), texture,
                                 IRect::MakeXYWH(0, 0, 2, 2),
                                 "Deferred texture overwrite"));
  EXPECT_TRUE(blit_pass->EncodeCommands());
  EXPECT_TRUE(context_base->GetCommandQueue()->Submit({command_buffer}).ok());
  EXPECT_FALSE(texture_generated);
  EXPECT_FALSE(texture_uploaded);

  worker->SetAllowed(true);
  EXPECT_TRUE(context->GetReactor()->React());
  EXPECT_TRUE(texture_generated);
  EXPECT_TRUE(texture_uploaded);
}

namespace {

struct CopyCustodyObservation {
  std::weak_ptr<void> source_owner;
  std::weak_ptr<void> destination_owner;
  size_t copies = 0;
};

thread_local CopyCustodyObservation* copy_custody_observation = nullptr;

void ObserveCustodyBlit(GLint sx0,
                        GLint sy0,
                        GLint sx1,
                        GLint sy1,
                        GLint dx0,
                        GLint dy0,
                        GLint dx1,
                        GLint dy1,
                        GLbitfield mask,
                        GLenum filter) {
  ASSERT_NE(copy_custody_observation, nullptr);
  EXPECT_FALSE(copy_custody_observation->source_owner.expired());
  EXPECT_FALSE(copy_custody_observation->destination_owner.expired());
  // The borrowed source is bottom-up; the owned destination is upright.
  EXPECT_EQ(sx0, 2);
  EXPECT_EQ(sy0, 7);
  EXPECT_EQ(sx1, 6);
  EXPECT_EQ(sy1, 5);
  EXPECT_EQ(dx0, 1);
  EXPECT_EQ(dy0, 2);
  EXPECT_EQ(dx1, 5);
  EXPECT_EQ(dy1, 4);
  EXPECT_EQ(mask, static_cast<GLbitfield>(GL_COLOR_BUFFER_BIT));
  EXPECT_EQ(filter, static_cast<GLenum>(GL_NEAREST));
  copy_custody_observation->copies++;
}

class CommandBufferGLESCopyCustody : public ::testing::Test {
 protected:
  void SetUp() override {
    auto implementation = std::make_unique<NiceMock<MockGLESImpl>>();
    ON_CALL(*implementation, GenTextures(_, _))
        .WillByDefault([](GLsizei count, GLuint* names) {
          for (GLsizei i = 0; i < count; i++)
            names[i] = 77 + i;
        });
    ON_CALL(*implementation, GenFramebuffers(_, _))
        .WillByDefault([](GLsizei count, GLuint* names) {
          for (GLsizei i = 0; i < count; i++)
            names[i] = 88 + i;
        });
    ON_CALL(*implementation, CheckFramebufferStatus(_))
        .WillByDefault(::testing::Return(GL_FRAMEBUFFER_COMPLETE));
    mock_ = MockGLES::Init(std::move(implementation), std::nullopt,
                           "OpenGL ES 3.0");
    copy_custody_observation = &observation_;
    auto gl = std::make_unique<ProcTableGLES>([](const char* name) -> void* {
      if (std::strcmp(name, "glBlitFramebuffer") == 0) {
        return reinterpret_cast<void*>(ObserveCustodyBlit);
      }
      return kMockResolverGLES(name);
    });
    context_ = ContextGLES::Create(Flags{}, std::move(gl),
                                   std::vector<std::shared_ptr<fml::Mapping>>{},
                                   false);
    ASSERT_TRUE(context_);
    base_ = std::static_pointer_cast<Context>(context_);
    worker_ = std::make_shared<ToggleWorker>(false);
    context_->AddReactorWorker(worker_);
  }

  void TearDown() override {
    // Also release queued custody after an assertion exits a test early.
    if (base_)
      base_->Shutdown();
    context_.reset();
    base_.reset();
    worker_.reset();
    mock_.reset();
    copy_custody_observation = nullptr;
  }

  Snapshot SourceSnapshot() {
    TextureDescriptor descriptor;
    descriptor.format = PixelFormat::kR8G8B8A8UNormInt;
    descriptor.size = {12, 10};
    descriptor.usage = TextureUsage::kRenderTarget;
    Snapshot snapshot;
    snapshot.texture =
        TextureGLES::WrapFBO(context_->GetReactor(), descriptor, 91);
    snapshot.resource_owner = std::make_shared<int>(17);
    observation_.source_owner = snapshot.resource_owner;
    return snapshot;
  }

  Snapshot DestinationSnapshot() {
    TextureDescriptor descriptor;
    descriptor.format = PixelFormat::kR8G8B8A8UNormInt;
    descriptor.size = {8, 6};
    descriptor.usage = TextureUsage::kRenderTarget | TextureUsage::kShaderRead;
    Snapshot snapshot;
    snapshot.texture = base_->GetResourceAllocator()->CreateTexture(
        descriptor, /*threadsafe=*/true);
    snapshot.resource_owner = std::make_shared<int>(23);
    observation_.destination_owner = snapshot.resource_owner;
    return snapshot;
  }

  void ExpectRetainedBeforeReaction() {
    EXPECT_EQ(observation_.copies, 0u);
    EXPECT_FALSE(observation_.source_owner.expired());
    EXPECT_FALSE(observation_.destination_owner.expired());
  }

  void ReactAndExpectReleased() {
    worker_->SetAllowed(true);
    ASSERT_TRUE(context_->GetReactor()->React());
    EXPECT_EQ(observation_.copies, 1u);
    EXPECT_TRUE(observation_.source_owner.expired());
    EXPECT_TRUE(observation_.destination_owner.expired());
  }

  std::shared_ptr<MockGLES> mock_;
  std::shared_ptr<ContextGLES> context_;
  std::shared_ptr<Context> base_;
  std::shared_ptr<ToggleWorker> worker_;
  CopyCustodyObservation observation_;
};

}  // namespace

TEST_F(CommandBufferGLESCopyCustody,
       DeferredCopyRetainsLogicalOwnersAfterAllCallerReferencesDrop) {
  auto source = SourceSnapshot();
  auto destination = DestinationSnapshot();
  ASSERT_TRUE(source.texture);
  ASSERT_TRUE(destination.texture);
  auto command = base_->CreateCommandBuffer();
  ASSERT_TRUE(command);
  auto pass = command->CreateBlitPass();
  ASSERT_TRUE(pass);
  ASSERT_TRUE(pass->TryRetainResource(source.resource_owner));
  ASSERT_TRUE(pass->TryRetainResource(destination.resource_owner));
  ASSERT_TRUE(pass->AddCopy(source.texture, destination.texture,
                            IRect::MakeXYWH(2, 3, 4, 2), IPoint{1, 2}));
  ASSERT_TRUE(pass->EncodeCommands());
  // No submit marker owns these logical claims. Encode itself queued the copy.
  command.reset();
  pass.reset();
  source = Snapshot{};
  destination = Snapshot{};
  ExpectRetainedBeforeReaction();
  ReactAndExpectReleased();
}

TEST_F(CommandBufferGLESCopyCustody,
       BlitOwnerOverflowRefusesEncodingWithoutQueuingTheCopy) {
  auto source = SourceSnapshot();
  auto destination = DestinationSnapshot();
  ASSERT_TRUE(source.texture);
  ASSERT_TRUE(destination.texture);
  auto command = base_->CreateCommandBuffer();
  ASSERT_TRUE(command);
  auto pass = command->CreateBlitPass();
  ASSERT_TRUE(pass);
  ASSERT_TRUE(pass->TryRetainResource(source.resource_owner));
  ASSERT_TRUE(pass->TryRetainResource(destination.resource_owner));
  ASSERT_TRUE(pass->AddCopy(source.texture, destination.texture,
                            IRect::MakeXYWH(2, 3, 4, 2), IPoint{1, 2}));
  for (size_t i = 2; i < BoundedResourceOwners::kCapacity; i++) {
    ASSERT_TRUE(pass->TryRetainResource(std::make_shared<size_t>(i)));
  }
  ASSERT_FALSE(pass->TryRetainResource(std::make_shared<size_t>(512)));
  EXPECT_FALSE(pass->HasValidResourceOwners());
  EXPECT_FALSE(pass->EncodeCommands());
  command.reset();
  pass.reset();
  source = Snapshot{};
  destination = Snapshot{};
  EXPECT_TRUE(observation_.source_owner.expired());
  EXPECT_TRUE(observation_.destination_owner.expired());
  worker_->SetAllowed(true);
  ASSERT_TRUE(context_->GetReactor()->React());
  EXPECT_EQ(observation_.copies, 0u);
}

TEST_F(CommandBufferGLESCopyCustody,
       QueuedCopyKeepsLogicalOwnersWhenLaterCommandSubmissionIsRejected) {
  auto source = SourceSnapshot();
  auto destination = DestinationSnapshot();
  ASSERT_TRUE(source.texture);
  ASSERT_TRUE(destination.texture);
  auto command = base_->CreateCommandBuffer();
  ASSERT_TRUE(command);
  auto pass = command->CreateBlitPass();
  ASSERT_TRUE(pass);
  ASSERT_TRUE(pass->TryRetainResource(source.resource_owner));
  ASSERT_TRUE(pass->TryRetainResource(destination.resource_owner));
  ASSERT_TRUE(command->TryRetainResource(source.resource_owner));
  ASSERT_TRUE(command->TryRetainResource(destination.resource_owner));
  ASSERT_TRUE(pass->AddCopy(source.texture, destination.texture,
                            IRect::MakeXYWH(2, 3, 4, 2), IPoint{1, 2}));
  ASSERT_TRUE(pass->EncodeCommands());
  // Poison only the command buffer after its blit has entered the reactor.
  // Refusing Submit cannot retroactively cancel that independently queued copy.
  for (size_t i = 2; i < BoundedResourceOwners::kCapacity; i++) {
    ASSERT_TRUE(command->TryRetainResource(std::make_shared<size_t>(i)));
  }
  ASSERT_FALSE(command->TryRetainResource(std::make_shared<size_t>(512)));
  size_t callbacks = 0;
  EXPECT_FALSE(base_->GetCommandQueue()
                   ->Submit({command},
                            [&](CommandBuffer::Status status) {
                              callbacks++;
                              EXPECT_EQ(status, CommandBuffer::Status::kError);
                            })
                   .ok());
  EXPECT_EQ(callbacks, 1u);
  command.reset();
  pass.reset();
  source = Snapshot{};
  destination = Snapshot{};
  ExpectRetainedBeforeReaction();
  ReactAndExpectReleased();
  EXPECT_EQ(callbacks, 1u);
}

}  // namespace testing
}  // namespace impeller
