// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/fml/build_config.h"

#if FML_OS_LINUX && IMPELLER_SUPPORTS_RENDERING
#include "flutter/shell/platform/embedder/embedder_opengl_selected_target.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

#include "flutter/impeller/display_list/aiks_context.h"
#include "flutter/testing/testing.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "impeller/renderer/backend/gles/context_gles.h"
#include "impeller/renderer/backend/gles/test/mock_gles.h"
#include "impeller/renderer/backend/gles/texture_gles.h"

namespace flutter::testing {
namespace {
using impeller::testing::MockGLES;
using impeller::testing::MockGLESImpl;
using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;

class CurrentContext final : public impeller::ReactorGLES::Worker {
 public:
  bool current = true;
  bool CanReactorReactOnCurrentThreadNow(
      const impeller::ReactorGLES&) const override {
    return current;
  }
};

void GenRenderbuffers(GLsizei count, GLuint* names) {
  for (GLsizei i = 0; i < count; i++) {
    names[i] = 71 + i;
  }
}
void GetRenderbufferParameter(GLenum, GLenum, GLint* value) {
  *value = 4;
}
GLboolean IsEnabled(GLenum) {
  return false;
}

class OpenGLSelectedTargetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto implementation = std::make_unique<NiceMock<MockGLESImpl>>();
    gl = implementation.get();
    ON_CALL(*gl, GetIntegerv(_, _)).WillByDefault([](GLenum key, GLint* value) {
      *value = key == GL_MAX_SAMPLES             ? 4
               : key == GL_MAX_RENDERBUFFER_SIZE ? 4096
                                                 : 0;
    });
    ON_CALL(*gl, GenFramebuffers(_, _))
        .WillByDefault([&](GLsizei count, GLuint* names) {
          for (GLsizei i = 0; i < count; i++) {
            names[i] = ++next_framebuffer;
          }
        });
    ON_CALL(*gl, GenTextures(_, _))
        .WillByDefault([](GLsizei count, GLuint* names) {
          for (GLsizei i = 0; i < count; i++) {
            names[i] = 91 + i;
          }
        });
    ON_CALL(*gl, CheckFramebufferStatus(_))
        .WillByDefault(Return(GL_FRAMEBUFFER_COMPLETE));
    mock = MockGLES::Init(std::move(implementation), std::nullopt,
                          "OpenGL ES 3.0");
    auto resolver = [](const char* name) -> void* {
      if (std::strcmp(name, "glGenRenderbuffers") == 0) {
        return reinterpret_cast<void*>(GenRenderbuffers);
      }
      if (std::strcmp(name, "glGetRenderbufferParameteriv") == 0) {
        return reinterpret_cast<void*>(GetRenderbufferParameter);
      }
      if (std::strcmp(name, "glIsEnabled") == 0) {
        return reinterpret_cast<void*>(IsEnabled);
      }
      return impeller::testing::kMockResolverGLES(name);
    };
    impeller::AvioAntialiasingConfig policy;
    policy.policy = impeller::AvioAntialiasingPolicy::kCoverage;
    policy.layer_sample_count = 1;
    policy.coverage_sample_count = 4;
    policy.coverage_region_max_bytes = 8 * 1024 * 1024;
    policy.layer_region_max_bytes = 8 * 1024 * 1024;
    context = impeller::ContextGLES::Create(
        impeller::Flags{},
        std::make_unique<impeller::ProcTableGLES>(resolver, true), {}, false,
        nullptr, policy);
    ASSERT_TRUE(context);
    ASSERT_TRUE(
        std::static_pointer_cast<impeller::Context>(context)->IsValid());
    current = std::make_shared<CurrentContext>();
    ASSERT_TRUE(context->AddReactorWorker(current));
    aiks = std::make_shared<impeller::AiksContext>(context, nullptr);
    store.struct_size = sizeof(store);
    store.type = kFlutterBackingStoreTypeOpenGL;
    store.open_gl.type = kFlutterOpenGLTargetTypeTexture;
    store.open_gl.texture = {
        .target = GL_TEXTURE_2D,
        .name = 123u,
        .format = GL_RGBA8,
        .user_data = this,
        .destruction_callback =
            [](void* data) {
              reinterpret_cast<OpenGLSelectedTargetTest*>(data)
                  ->order.push_back(2);
            },
        .width = 32,
        .height = 24,
    };
    synchronization = {
        .struct_size = sizeof(synchronization),
        .user_data = this,
        .export_render_complete_sync_fd =
            [](void* data) {
              auto& fixture =
                  *reinterpret_cast<OpenGLSelectedTargetTest*>(data);
              fixture.exports++;
              fixture.order.push_back(1);
              return std::exchange(fixture.export_fd, -1);
            },
    };
    store.open_gl_synchronization = &synchronization;
    config.struct_size = sizeof(config);
    config.size = {32, 24};
    synchronization.selected_target_id =
        context->RegisterExternalTarget(123u, {32, 24});
    ASSERT_NE(synchronization.selected_target_id, 0u);
  }

  void TearDown() override {
    if (std::static_pointer_cast<impeller::Context>(context)->IsValid() &&
        current->current) {
      EXPECT_TRUE(context->RetireExternalRenderTargets());
    }
  }

  std::unique_ptr<EmbedderRenderTarget> MakeTarget() {
    return MakeOpenGLSelectedRenderTarget(
        store, [&] { order.push_back(3); }, aiks, config);
  }

  std::shared_ptr<MockGLES> mock;
  MockGLESImpl* gl = nullptr;
  std::shared_ptr<impeller::ContextGLES> context;
  std::shared_ptr<CurrentContext> current;
  std::shared_ptr<impeller::AiksContext> aiks;
  FlutterBackingStore store = {};
  FlutterOpenGLBackingStoreSynchronization synchronization = {};
  FlutterBackingStoreConfig config = {};
  std::vector<int> order;
  GLuint next_framebuffer = 1000;
  int export_fd = -1;
  int exports = 0;
};

TEST_F(OpenGLSelectedTargetTest, RetainedTextureLoadsWithoutOwningItsStorage) {
  FlutterRegion damage{.struct_size = sizeof(FlutterRegion)};
  FlutterBackingStoreContentState contents{.struct_size = sizeof(contents),
                                           .target_identifier = 19,
                                           .content_epoch = 4,
                                           .preserved_contents = true,
                                           .existing_damage = &damage};
  store.content_state = &contents;
  EXPECT_CALL(*gl, GenTextures(_, _)).Times(0);
  EXPECT_CALL(*gl, TexImage2D(_, _, _, _, _, _, _, _, _)).Times(0);
  EXPECT_CALL(*gl, DeleteTextures(_, _)).Times(0);
  auto target = MakeTarget();
  ASSERT_TRUE(target);
  EXPECT_FALSE(target->RasterReplacesWholeTarget());
  EXPECT_TRUE(target->RequiresRenderCompleteSyncFD());
  const auto allocated = next_framebuffer;
  EXPECT_EQ(target->GetRenderTargetSize(), DlISize(32, 24));
  EXPECT_EQ(next_framebuffer, allocated);
  auto* render = target->GetImpellerRenderTarget();
  ASSERT_TRUE(render);
  EXPECT_EQ(next_framebuffer, allocated);
  EXPECT_EQ(render, target->GetImpellerRenderTarget());
  EXPECT_EQ(next_framebuffer, allocated);
  EXPECT_EQ(render->GetColorAttachment(0).load_action,
            impeller::LoadAction::kLoad);
  EXPECT_EQ(render->GetSampleCount(), impeller::SampleCount::kCount1);
  auto retained_source = render->GetColorAttachment(0).texture;
  target.reset();
  EXPECT_TRUE(order.empty());
  retained_source.reset();
  EXPECT_EQ(order, (std::vector<int>{2, 3}));
  EXPECT_EQ(exports, 0);
  EXPECT_TRUE(context->GetReactor()->React());
}

TEST_F(OpenGLSelectedTargetTest, UnchangedLeaseDoesNotAllocateOrExport) {
  EXPECT_CALL(*gl, GenFramebuffers(_, _)).Times(0);
  EXPECT_CALL(*gl, GenTextures(_, _)).Times(0);
  auto target = MakeTarget();
  ASSERT_TRUE(target);
  EXPECT_FALSE(target->RasterReplacesWholeTarget());
  target.reset();
  EXPECT_EQ(exports, 0);
  EXPECT_EQ(order, (std::vector<int>{2, 3}));
}

TEST_F(OpenGLSelectedTargetTest, CompletionRunsAfterQueuedWorkAndOnlyOnce) {
  auto target = MakeTarget();
  ASSERT_TRUE(target);
  // A pipe is only an fd-ownership witness here, not a claimed GPU fence.
  int pipe_fds[2];
  ASSERT_EQ(pipe(pipe_fds), 0);
  fml::UniqueFD writer(pipe_fds[1]);
  export_fd = pipe_fds[0];
  ASSERT_TRUE(context->GetReactor()->AddOperation(
      [&](const impeller::ReactorGLES&) { order.push_back(0); }, true));
  {
    auto fd = target->TakeRenderCompleteSyncFD();
    EXPECT_EQ(fd.get(), pipe_fds[0]);
    EXPECT_EQ(order, (std::vector<int>{0, 1}));
    EXPECT_FALSE(target->TakeRenderCompleteSyncFD().is_valid());
    EXPECT_EQ(exports, 1);
    EXPECT_EQ(context->RetireExternalTarget(synchronization.selected_target_id),
              impeller::ContextGLES::ExternalTargetRetirement::kInUse);
  }
  EXPECT_EQ(fcntl(pipe_fds[0], F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
  target.reset();
  EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3}));
  EXPECT_EQ(context->RetireExternalTarget(synchronization.selected_target_id),
            impeller::ContextGLES::ExternalTargetRetirement::kSuccess);
  EXPECT_TRUE(context->IsSafeToDestroyNativeResources());
}

TEST_F(OpenGLSelectedTargetTest, MissingCompletionHasNoIdleFallback) {
  auto target = MakeTarget();
  ASSERT_TRUE(target);
  EXPECT_TRUE(target->RequiresRenderCompleteSyncFD());
  EXPECT_FALSE(target->TakeRenderCompleteSyncFD().is_valid());
  EXPECT_FALSE(target->TakeRenderCompleteSyncFD().is_valid());
  EXPECT_EQ(exports, 1);
  EXPECT_FALSE(context->IsSafeToDestroyNativeResources());
  std::static_pointer_cast<impeller::Context>(context)->Shutdown();
  EXPECT_FALSE(context->IsSafeToDestroyNativeResources());
}

TEST_F(OpenGLSelectedTargetTest, LostCurrentContextCannotCertifyCompletion) {
  auto target = MakeTarget();
  ASSERT_TRUE(target);
  current->current = false;
  EXPECT_FALSE(target->TakeRenderCompleteSyncFD().is_valid());
  current->current = true;
  EXPECT_FALSE(target->TakeRenderCompleteSyncFD().is_valid());
  EXPECT_EQ(exports, 0);
  EXPECT_FALSE(context->IsSafeToDestroyNativeResources());
}

TEST_F(OpenGLSelectedTargetTest, AbandonedReactorCannotExportCompletion) {
  auto target = MakeTarget();
  ASSERT_TRUE(target);
  bool executed = false;
  ASSERT_TRUE(context->GetReactor()->AddOperation(
      [&](const impeller::ReactorGLES&) { executed = true; }, true));
  std::static_pointer_cast<impeller::Context>(context)->Shutdown();
  EXPECT_FALSE(target->TakeRenderCompleteSyncFD().is_valid());
  EXPECT_FALSE(executed);
  EXPECT_EQ(exports, 0);
  EXPECT_FALSE(context->IsSafeToDestroyNativeResources());
}

TEST_F(OpenGLSelectedTargetTest, ContextLossAfterPartialWorkCannotExport) {
  auto target = MakeTarget();
  ASSERT_TRUE(target);
  bool issued = false;
  ASSERT_TRUE(context->GetReactor()->AddOperation(
      [&](const impeller::ReactorGLES&) {
        issued = true;
        current->current = false;
      },
      true));
  EXPECT_FALSE(target->TakeRenderCompleteSyncFD().is_valid());
  EXPECT_TRUE(issued);
  EXPECT_EQ(exports, 0);
  EXPECT_FALSE(context->IsSafeToDestroyNativeResources());
  current->current = true;
  target.reset();
  EXPECT_EQ(context->RetireExternalTarget(synchronization.selected_target_id),
            impeller::ContextGLES::ExternalTargetRetirement::kFailed);
}

TEST_F(OpenGLSelectedTargetTest, MissingOrTruncatedSynchronizationRefuses) {
  store.open_gl_synchronization = nullptr;
  EXPECT_FALSE(MakeTarget());
  store.open_gl_synchronization = &synchronization;
  synchronization.struct_size = offsetof(
      FlutterOpenGLBackingStoreSynchronization, export_render_complete_sync_fd);
  EXPECT_FALSE(MakeTarget());
  synchronization.struct_size = sizeof(synchronization);
  synchronization.export_render_complete_sync_fd = nullptr;
  EXPECT_FALSE(MakeTarget());
  EXPECT_TRUE(order.empty());  // Acquisition guard retains callback custody.
}

TEST_F(OpenGLSelectedTargetTest, InvalidTextureLayoutRefusesBeforeRaster) {
  store.open_gl.texture.width = 31;
  EXPECT_FALSE(MakeTarget());
  store.open_gl.texture.width = 32;
  store.open_gl.texture.name = 0;
  EXPECT_FALSE(MakeTarget());
  store.open_gl.texture.name = 123;
  store.open_gl.texture.target = GL_TEXTURE_EXTERNAL_OES;
  EXPECT_FALSE(MakeTarget());
  EXPECT_EQ(exports, 0);
}

TEST_F(OpenGLSelectedTargetTest, BorrowedFramebufferIsNeverDeleted) {
  ASSERT_EQ(context->RetireExternalTarget(synchronization.selected_target_id),
            impeller::ContextGLES::ExternalTargetRetirement::kSuccess);
  store.open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;
  store.open_gl.framebuffer = {.target = GL_RGBA8, .name = 777u};
  EXPECT_CALL(*gl, GenFramebuffers(_, _)).Times(0);
  EXPECT_CALL(*gl, DeleteFramebuffers(_, _)).Times(0);
  auto target = MakeTarget();
  ASSERT_TRUE(target);
  auto* render = target->GetImpellerRenderTarget();
  ASSERT_TRUE(render);
  EXPECT_EQ(render->GetColorAttachment(0).load_action,
            impeller::LoadAction::kClear);
  target.reset();
  EXPECT_TRUE(context->GetReactor()->React());
  EXPECT_EQ(order, (std::vector<int>{3}));
}

TEST_F(OpenGLSelectedTargetTest, IncompleteFramebufferFailsBeforeSubmission) {
  store.open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;
  store.open_gl.framebuffer = {.target = GL_RGBA8, .name = 777u};
  EXPECT_CALL(*gl, CheckFramebufferStatus(GL_FRAMEBUFFER))
      .WillOnce(Return(GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT));
  auto target = MakeTarget();
  ASSERT_TRUE(target);
  EXPECT_EQ(target->GetImpellerRenderTarget(), nullptr);
  EXPECT_EQ(target->GetImpellerRenderTarget(), nullptr);
  EXPECT_EQ(exports, 0);
  target.reset();
  EXPECT_EQ(order, (std::vector<int>{3}));
}

TEST_F(OpenGLSelectedTargetTest, ManyLeasesReuseOneMembershipFramebuffer) {
  EXPECT_CALL(*gl, GenFramebuffers(_, _)).Times(0);
  EXPECT_CALL(*gl, GenTextures(_, _)).Times(0);
  EXPECT_CALL(*gl, TexImage2D(_, _, _, _, _, _, _, _, _)).Times(0);
  const auto framebuffer = next_framebuffer;
  for (int lease = 0; lease < 1000; lease++) {
    auto target = MakeTarget();
    ASSERT_TRUE(target);
    auto* render = target->GetImpellerRenderTarget();
    ASSERT_TRUE(render);
    auto source = render->GetColorAttachment(0).texture;
    EXPECT_EQ(impeller::TextureGLES::Cast(*source).GetFBO(), framebuffer);
  }
  EXPECT_EQ(next_framebuffer, framebuffer);
  EXPECT_EQ(order.size(), 2000u);
  const auto report = context->GetAvioRenderResourceReport(false);
  const auto end = report.entries.begin() + report.entries_count;
  const auto entry =
      std::find_if(report.entries.begin(), end, [](const auto& value) {
        return value.kind_id ==
               kFlutterAvioRenderResourceExternalTargetBindings;
      });
  ASSERT_NE(entry, end);
  EXPECT_EQ(entry->usage.entries, 1u);
  EXPECT_EQ(entry->usage.created_entries, 1u);
  EXPECT_EQ(entry->usage.leased_entries, 0u);
  EXPECT_EQ(entry->fields_supported, impeller::kAvioResourceFieldCounts |
                                         impeller::kAvioResourceFieldLeases);
  EXPECT_EQ(entry->unsupported_reason_id,
            impeller::kAvioResourceReasonPhysicalAllocationUnavailable);
}

TEST_F(OpenGLSelectedTargetTest, RetirementDetachesBeforeMembershipDisappears) {
  auto target = MakeTarget();
  ASSERT_TRUE(target);
  auto retained =
      target->GetImpellerRenderTarget()->GetColorAttachment(0).texture;
  target.reset();
  const auto id = synchronization.selected_target_id;
  EXPECT_EQ(context->RetireExternalTarget(id),
            impeller::ContextGLES::ExternalTargetRetirement::kInUse);
  EXPECT_TRUE(order.empty());
  retained.reset();
  EXPECT_EQ(order, (std::vector<int>{2, 3}));
  {
    ::testing::InSequence ordered;
    EXPECT_CALL(*gl, FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_TEXTURE_2D, 0, 0));
    EXPECT_CALL(*gl, DeleteFramebuffers(1, _));
  }
  EXPECT_EQ(context->RetireExternalTarget(id),
            impeller::ContextGLES::ExternalTargetRetirement::kSuccess);
  EXPECT_FALSE(context->AcquireExternalTarget(id, 123u, {32, 24}));
}

TEST_F(OpenGLSelectedTargetTest,
       RecycledTextureNameDoesNotReuseRetiredIdentity) {
  const auto previous = synchronization.selected_target_id;
  ASSERT_EQ(context->RetireExternalTarget(previous),
            impeller::ContextGLES::ExternalTargetRetirement::kSuccess);
  const auto next = context->RegisterExternalTarget(123u, {32, 24});
  ASSERT_NE(next, 0u);
  EXPECT_NE(previous, next);
  EXPECT_FALSE(MakeTarget());
  synchronization.selected_target_id = next;
  EXPECT_TRUE(MakeTarget());
}

TEST_F(OpenGLSelectedTargetTest, MissingMembershipNeverAllocatesOnFrame) {
  EXPECT_CALL(*gl, GenFramebuffers(_, _)).Times(0);
  synchronization.selected_target_id = 0;
  EXPECT_FALSE(MakeTarget());
  synchronization.struct_size =
      offsetof(FlutterOpenGLBackingStoreSynchronization, selected_target_id);
  EXPECT_FALSE(MakeTarget());
  EXPECT_TRUE(order.empty());
}

TEST_F(OpenGLSelectedTargetTest, ShutdownWithoutCurrentRetirementIsUnsafe) {
  current->current = false;
  EXPECT_FALSE(context->RetireExternalRenderTargets());
  std::static_pointer_cast<impeller::Context>(context)->Shutdown();
  EXPECT_FALSE(context->IsSafeToDestroyNativeResources());
}

TEST_F(OpenGLSelectedTargetTest, DebugErrorConsumptionCannotCertifyCompletion) {
  auto target = MakeTarget();
  ASSERT_TRUE(target);
  {
    // GL_CONTEXT_LOST may already have been consumed by debug call logging.
    impeller::AutoErrorCheck error(
        []() -> GLenum { return 0x0507; }, "injected context loss",
        context->GetReactor()->GetProcTable().Clear.captured_error);
  }
  EXPECT_FALSE(target->TakeRenderCompleteSyncFD().is_valid());
  EXPECT_EQ(exports, 0);
  EXPECT_FALSE(context->IsSafeToDestroyNativeResources());
  target.reset();
  std::static_pointer_cast<impeller::Context>(context)->Shutdown();
}

TEST_F(OpenGLSelectedTargetTest, PoisonedEpochNeverRetriesNativeDeletion) {
  const auto id = synchronization.selected_target_id;
  context->MarkExternalCompletionUnobservable();
  EXPECT_CALL(*gl, DeleteFramebuffers(_, _)).Times(0);
  EXPECT_CALL(*gl, GenFramebuffers(_, _)).Times(0);
  EXPECT_EQ(context->RetireExternalTarget(id),
            impeller::ContextGLES::ExternalTargetRetirement::kFailed);
  EXPECT_FALSE(context->AcquireExternalTarget(id, 123u, {32, 24}));
  EXPECT_EQ(context->RegisterExternalTarget(456u, {32, 24}), 0u);
  EXPECT_FALSE(context->IsSafeToDestroyNativeResources());
}

TEST_F(OpenGLSelectedTargetTest,
       PoisonedEpochRejectsExistingAndBorrowedTargets) {
  auto existing = MakeTarget();
  ASSERT_TRUE(existing);
  context->MarkExternalCompletionUnobservable();
  EXPECT_FALSE(existing->GetImpellerRenderTarget());
  EXPECT_FALSE(existing->TakeRenderCompleteSyncFD().is_valid());
  EXPECT_EQ(exports, 0);
  store.open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;
  store.open_gl.framebuffer = {.target = GL_RGBA8, .name = 777u};
  EXPECT_FALSE(MakeTarget());
  EXPECT_EQ(exports, 0);
  EXPECT_FALSE(context->IsSafeToDestroyNativeResources());
}

TEST_F(OpenGLSelectedTargetTest, UncertainNativeDeletionCannotBeRetried) {
  const auto id = synchronization.selected_target_id;
  EXPECT_CALL(*gl, DeleteFramebuffers(1, _))
      .WillOnce([&](GLsizei, const GLuint*) {
        context->GetReactor()->GetProcTable().Clear.captured_error->store(
            GL_INVALID_OPERATION);
      });
  EXPECT_EQ(context->RetireExternalTarget(id),
            impeller::ContextGLES::ExternalTargetRetirement::kFailed);
  EXPECT_EQ(context->RetireExternalTarget(id),
            impeller::ContextGLES::ExternalTargetRetirement::kFailed);
  EXPECT_FALSE(context->AcquireExternalTarget(id, 123u, {32, 24}));
  EXPECT_FALSE(context->IsSafeToDestroyNativeResources());
}

TEST_F(OpenGLSelectedTargetTest, MembershipRejectsAnotherContextOwnerThread) {
  const auto id = synchronization.selected_target_id;
  EXPECT_CALL(*gl, GenFramebuffers(_, _)).Times(0);
  std::thread other([&] {
    EXPECT_FALSE(context->AcquireExternalTarget(id, 123u, {32, 24}));
    EXPECT_EQ(context->RegisterExternalTarget(456u, {32, 24}), 0u);
    EXPECT_EQ(context->RetireExternalTarget(id),
              impeller::ContextGLES::ExternalTargetRetirement::kFailed);
  });
  other.join();
  EXPECT_TRUE(context->AcquireExternalTarget(id, 123u, {32, 24}));
}

}  // namespace
}  // namespace flutter::testing
#endif  // FML_OS_LINUX && IMPELLER_SUPPORTS_RENDERING
