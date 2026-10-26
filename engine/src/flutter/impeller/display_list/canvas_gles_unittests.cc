// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string_view>
#include <vector>

#include "flutter/fml/closure.h"
#include "flutter/fml/mapping.h"
#include "flutter/testing/testing.h"  // IWYU pragma: keep
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "impeller/display_list/backdrop_filter_support.h"
#include "impeller/display_list/canvas.h"
#include "impeller/display_list/coverage_backdrop.h"
#include "impeller/display_list/dl_dispatcher.h"
#include "impeller/entity/avio_coverage_region.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/contents/filters/gaussian_blur_filter_contents.h"
#include "impeller/entity/geometry/rect_geometry.h"
#include "impeller/entity/gles/entity_shaders_gles.h"
#include "impeller/entity/gles/framebuffer_blend_shaders_gles.h"
#include "impeller/entity/gles/modern_shaders_gles.h"
#include "impeller/renderer/backend/gles/context_gles.h"
#include "impeller/renderer/backend/gles/test/mock_gles.h"
#include "impeller/renderer/backend/gles/texture_gles.h"
#include "impeller/renderer/render_target.h"
#include "impeller/typographer/backends/skia/typographer_context_skia.h"

namespace impeller {
namespace testing {

namespace {

using ::testing::_;
using ::testing::IsEmpty;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::SetArgPointee;

class MockReactorWorker final : public ReactorGLES::Worker {
 public:
  bool current = true;
  // |ReactorGLES::Worker|
  bool CanReactorReactOnCurrentThreadNow(
      const ReactorGLES& reactor) const override {
    return current;
  }
};

std::vector<std::shared_ptr<fml::Mapping>> ShaderLibraryMappings() {
  return {
      std::make_shared<fml::NonOwnedMapping>(
          impeller_entity_shaders_gles_data,
          impeller_entity_shaders_gles_length),
      std::make_shared<fml::NonOwnedMapping>(
          impeller_modern_shaders_gles_data,
          impeller_modern_shaders_gles_length),
      std::make_shared<fml::NonOwnedMapping>(
          impeller_framebuffer_blend_shaders_gles_data,
          impeller_framebuffer_blend_shaders_gles_length),
  };
}

/// The sampler uniforms every mocked program reports as active, so the backend
/// binds the textures the entity shaders sample from.
constexpr std::array<std::string_view, 4> kSamplerUniforms = {
    "texture_sampler",
    "texture_sampler_dst",
    "texture_sampler_src",
    "color_source_sampler",
};

/// Drives a |MockGLESImpl| with just enough OpenGL ES state tracking to tell
/// which textures each draw samples and which texture it renders into.
///
/// A draw that samples the color attachment of the framebuffer it renders to
/// is a feedback loop, which is undefined behavior. ANGLE on Direct3D 11, for
/// example, samples zeros.
class FeedbackLoopDetector {
 public:
  explicit FeedbackLoopDetector(NiceMock<MockGLESImpl>& gl) {
    // Pipelines compile, link, and report the sampler uniforms as active.
    ON_CALL(gl, CreateShader(_)).WillByDefault(Return(1));
    ON_CALL(gl, CreateProgram()).WillByDefault(Return(1));
    ON_CALL(gl, IsProgram(_)).WillByDefault(Return(GL_TRUE));
    ON_CALL(gl, GetShaderiv(_, GL_COMPILE_STATUS, _))
        .WillByDefault(SetArgPointee<2>(GL_TRUE));
    ON_CALL(gl, GetProgramiv(_, _, _))
        .WillByDefault([](GLuint program, GLenum pname, GLint* params) {
          switch (pname) {
            case GL_LINK_STATUS:
              *params = GL_TRUE;
              break;
            case GL_ACTIVE_UNIFORMS:
              *params = kSamplerUniforms.size();
              break;
            case GL_ACTIVE_UNIFORM_MAX_LENGTH:
              *params = 64;
              break;
          }
        });
    ON_CALL(gl, GetActiveUniform(_, _, _, _, _, _, _))
        .WillByDefault([](GLuint program, GLuint index, GLsizei buf_size,
                          GLsizei* length, GLint* size, GLenum* type,
                          GLchar* name) {
          std::string_view uniform = kSamplerUniforms[index];
          std::memcpy(name, uniform.data(), uniform.size());
          name[uniform.size()] = '\0';
          *length = uniform.size();
          *size = 1;
          *type = GL_SAMPLER_2D;
        });
    ON_CALL(gl, GetUniformLocation(_, _))
        .WillByDefault([](GLuint program, const GLchar* name) -> GLint {
          for (size_t i = 0; i < kSamplerUniforms.size(); i++) {
            if (kSamplerUniforms[i] == name) {
              return i;
            }
          }
          return -1;
        });
    ON_CALL(gl, GetIntegerv(_, _)).WillByDefault([](GLenum name, GLint* value) {
      if (name == GL_MAX_TEXTURE_IMAGE_UNITS ||
          name == GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS) {
        *value = 8;
      }
    });

    // Every texture and framebuffer gets a distinct name.
    ON_CALL(gl, GenTextures(_, _))
        .WillByDefault(
            [this](GLsizei n, GLuint* names) { GenNames(n, names); });
    ON_CALL(gl, GenFramebuffers(_, _))
        .WillByDefault(
            [this](GLsizei n, GLuint* names) { GenNames(n, names); });
    ON_CALL(gl, IsTexture(_)).WillByDefault(Return(GL_TRUE));
    ON_CALL(gl, CheckFramebufferStatus(_))
        .WillByDefault(Return(GL_FRAMEBUFFER_COMPLETE));

    // Track the framebuffer being rendered to and its color attachment.
    ON_CALL(gl, BindFramebuffer(_, _))
        .WillByDefault([this](GLenum target, GLuint framebuffer) {
          std::scoped_lock lock(mutex_);
          if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
            draw_framebuffer_ = framebuffer;
          }
          if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) {
            read_framebuffer_ = framebuffer;
          }
        });
    ON_CALL(gl, FramebufferTexture2D(_, GL_COLOR_ATTACHMENT0, _, _, _))
        .WillByDefault([this](GLenum target, GLenum attachment,
                              GLenum textarget, GLuint texture, GLint level) {
          std::scoped_lock lock(mutex_);
          color_attachments_[target == GL_READ_FRAMEBUFFER
                                 ? read_framebuffer_
                                 : draw_framebuffer_] = texture;
        });

    // Track the textures bound to sampler uniforms.
    ON_CALL(gl, ActiveTexture(_)).WillByDefault([this](GLenum texture) {
      std::scoped_lock lock(mutex_);
      active_unit_ = texture - GL_TEXTURE0;
    });
    ON_CALL(gl, BindTexture(_, _))
        .WillByDefault([this](GLenum target, GLuint texture) {
          std::scoped_lock lock(mutex_);
          texture_units_[active_unit_] = texture;
        });
    ON_CALL(gl, Uniform1i(_, _))
        .WillByDefault([this](GLint location, GLint v0) {
          std::scoped_lock lock(mutex_);
          sampled_textures_.insert(texture_units_[v0]);
        });

    ON_CALL(gl, DrawArrays(_, _, _)).WillByDefault([this](auto...) { Draw(); });
    ON_CALL(gl, DrawElements(_, _, _, _)).WillByDefault([this](auto...) {
      Draw();
    });
  }

  /// The number of draws that sampled at least one texture.
  size_t GetSamplingDrawCount() const {
    std::scoped_lock lock(mutex_);
    return sampling_draw_count_;
  }

  /// The textures that were sampled by a draw rendering into them.
  std::vector<GLuint> GetFeedbackLoopTextures() const {
    std::scoped_lock lock(mutex_);
    return feedback_loop_textures_;
  }

  struct Blit {
    GLuint source_fbo;
    GLuint destination_texture;
    std::array<GLint, 8> coordinates;
    size_t serial;
    size_t clear_serial;
    size_t prior_copy_serial;
  };

  void RegisterBorrowedFBO(GLuint fbo, GLuint texture) {
    std::scoped_lock lock(mutex_);
    color_attachments_[fbo] = texture;
  }

  void RecordClear(GLbitfield bits) {
    std::scoped_lock lock(mutex_);
    if (bits & GL_COLOR_BUFFER_BIT) {
      const auto texture = color_attachments_[draw_framebuffer_];
      cleared_[texture] = ++serial_;
      ++clear_counts_[texture];
    }
  }

  size_t GetColorClearCount(GLuint texture) const {
    std::scoped_lock lock(mutex_);
    auto found = clear_counts_.find(texture);
    return found == clear_counts_.end() ? 0 : found->second;
  }

  void RecordBlit(const std::array<GLint, 8>& coordinates) {
    std::scoped_lock lock(mutex_);
    auto destination = color_attachments_[draw_framebuffer_];
    blits_.push_back({read_framebuffer_, destination, coordinates, ++serial_,
                      cleared_[destination], copied_[destination]});
    copied_[destination] = serial_;
  }

  std::vector<Blit> RootBlits(GLuint root) const {
    std::scoped_lock lock(mutex_);
    std::vector<Blit> result;
    for (const auto& blit : blits_) {
      if (blit.source_fbo == root) {
        result.push_back(blit);
      }
    }
    return result;
  }

 private:
  mutable std::mutex mutex_;
  GLuint next_name_ = 1;
  GLuint draw_framebuffer_ = 0;
  GLuint read_framebuffer_ = 0;
  size_t serial_ = 0;
  std::map<GLuint, size_t> cleared_;
  std::map<GLuint, size_t> clear_counts_;
  std::map<GLuint, size_t> copied_;
  std::vector<Blit> blits_;
  GLenum active_unit_ = 0;
  std::map<GLuint, GLuint> color_attachments_;
  std::map<GLenum, GLuint> texture_units_;
  std::set<GLuint> sampled_textures_;
  size_t sampling_draw_count_ = 0;
  std::vector<GLuint> feedback_loop_textures_;

  void GenNames(GLsizei n, GLuint* names) {
    std::scoped_lock lock(mutex_);
    for (GLsizei i = 0; i < n; i++) {
      names[i] = next_name_++;
    }
  }

  void Draw() {
    std::scoped_lock lock(mutex_);
    if (!sampled_textures_.empty()) {
      sampling_draw_count_++;
    }
    auto target = color_attachments_.find(draw_framebuffer_);
    if (target != color_attachments_.end() &&
        sampled_textures_.contains(target->second)) {
      feedback_loop_textures_.push_back(target->second);
    }
    sampled_textures_.clear();
  }
};

}  // namespace

// Emulates a device without offscreen MSAA and without framebuffer fetch, for
// example ANGLE on a Direct3D 11 feature level 10_0 device. The mock driver
// advertises neither GL_EXT_multisampled_render_to_texture nor
// GL_EXT_shader_framebuffer_fetch, so the capabilities come from the real
// OpenGL ES backend instead of being overridden.
TEST(CanvasGLESTest, AdvancedBlendWithoutOffscreenMSAAHasNoFeedbackLoop) {
  auto mock_gles_impl = std::make_unique<NiceMock<MockGLESImpl>>();
  FeedbackLoopDetector detector(*mock_gles_impl);
  std::shared_ptr<MockGLES> mock_gles = MockGLES::Init(
      std::move(mock_gles_impl), /*extensions=*/std::nullopt, "OpenGL ES 2.0");

  std::shared_ptr<ContextGLES> context_gles = ContextGLES::Create(
      Flags{}, std::make_unique<ProcTableGLES>(kMockResolverGLES),
      ShaderLibraryMappings(),
      /*enable_gpu_tracing=*/false);
  ASSERT_TRUE(context_gles);
  auto worker = std::make_shared<MockReactorWorker>();
  context_gles->AddReactorWorker(worker);
  std::shared_ptr<Context> context = context_gles;
  ASSERT_TRUE(context->IsValid());

  const std::shared_ptr<const Capabilities>& capabilities =
      context->GetCapabilities();
  ASSERT_FALSE(capabilities->SupportsOffscreenMSAA());
  ASSERT_FALSE(capabilities->SupportsFramebufferFetch());

  ContentContext content_context(context, TypographerContextSkia::Make());
  ASSERT_TRUE(content_context.IsValid());

  TextureDescriptor onscreen_desc;
  onscreen_desc.size = {100, 100};
  onscreen_desc.format = capabilities->GetDefaultColorFormat();
  onscreen_desc.usage = TextureUsage::kRenderTarget | TextureUsage::kShaderRead;
  onscreen_desc.storage_mode = StorageMode::kDevicePrivate;
  std::shared_ptr<Texture> onscreen =
      context->GetResourceAllocator()->CreateTexture(onscreen_desc);
  ASSERT_TRUE(onscreen);

  ColorAttachment color0;
  color0.texture = onscreen;
  color0.load_action = LoadAction::kClear;
  color0.store_action = StoreAction::kStore;
  RenderTarget render_target;
  render_target.SetColorAttachment(color0, 0);
  render_target.SetupDepthStencilAttachments(
      *context, *context->GetResourceAllocator(), onscreen_desc.size,
      /*msaa_enabled=*/false, "Feedback-loop test");

  Canvas canvas(content_context, render_target, /*is_onscreen=*/true,
                /*requires_readback=*/true, Rect::MakeLTRB(0, 0, 100, 100));
  canvas.Save(/*total_content_depth=*/2);
  canvas.DrawRect(Rect::MakeXYWH(10, 10, 40, 40),
                  Paint{.color = Color::Blue()});
  // An advanced blend without framebuffer fetch flips the backdrop. Without
  // offscreen MSAA the pass after the flip renders into the same texture the
  // backdrop is read from.
  canvas.DrawRect(
      Rect::MakeXYWH(20, 20, 40, 40),
      Paint{.color = Color::Orange(), .blend_mode = BlendMode::kMultiply});
  canvas.Restore();
  canvas.EndReplay();

  // The blend samples the backdrop, so the detector must have seen it.
  EXPECT_GT(detector.GetSamplingDrawCount(), 0u);
  EXPECT_THAT(detector.GetFeedbackLoopTextures(), IsEmpty());
}

namespace {
thread_local FeedbackLoopDetector* coverage_detector = nullptr;
thread_local bool coverage_scissor = false;
void CoverageGenRenderbuffers(GLsizei count, GLuint* names) {
  static GLuint next = 50000;
  for (GLsizei i = 0; i < count; ++i) {
    names[i] = ++next;
  }
}
void CoverageRenderbufferParameter(GLenum, GLenum, GLint* value) {
  *value = 4;
}
GLboolean CoverageIsEnabled(GLenum name) {
  return name == GL_SCISSOR_TEST && coverage_scissor ? GL_TRUE : GL_FALSE;
}
void CoverageEnable(GLenum name) {
  if (name == GL_SCISSOR_TEST)
    coverage_scissor = true;
}
void CoverageDisable(GLenum name) {
  if (name == GL_SCISSOR_TEST)
    coverage_scissor = false;
}
void CoverageClear(GLbitfield bits) {
  if (bits & GL_COLOR_BUFFER_BIT) {
    EXPECT_FALSE(coverage_scissor);
  }
  coverage_detector->RecordClear(bits);
}
void CoverageBlit(GLint sx0,
                  GLint sy0,
                  GLint sx1,
                  GLint sy1,
                  GLint dx0,
                  GLint dy0,
                  GLint dx1,
                  GLint dy1,
                  GLbitfield mask,
                  GLenum filter) {
  EXPECT_EQ(mask, static_cast<GLbitfield>(GL_COLOR_BUFFER_BIT));
  EXPECT_EQ(filter, static_cast<GLenum>(GL_NEAREST));
  coverage_detector->RecordBlit({sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1});
}
void* CoverageResolver(const char* name) {
  if (std::strcmp(name, "glGenRenderbuffers") == 0) {
    return reinterpret_cast<void*>(CoverageGenRenderbuffers);
  }
  if (std::strcmp(name, "glGetRenderbufferParameteriv") == 0) {
    return reinterpret_cast<void*>(CoverageRenderbufferParameter);
  }
  if (std::strcmp(name, "glIsEnabled") == 0) {
    return reinterpret_cast<void*>(CoverageIsEnabled);
  }
  if (std::strcmp(name, "glEnable") == 0) {
    return reinterpret_cast<void*>(CoverageEnable);
  }
  if (std::strcmp(name, "glDisable") == 0) {
    return reinterpret_cast<void*>(CoverageDisable);
  }
  if (std::strcmp(name, "glClear") == 0) {
    return reinterpret_cast<void*>(CoverageClear);
  }
  if (std::strcmp(name, "glBlitFramebuffer") == 0) {
    return reinterpret_cast<void*>(CoverageBlit);
  }
  return kMockResolverGLES(name);
}

class CanvasCoverageFramebufferTest : public ::testing::Test {
 protected:
  static constexpr GLuint kRoot = 90000;
  static constexpr GLuint kBorrowedTexture = 90001;
  const ISize size = {512, 320};

  void SetUp() override {
    auto implementation = std::make_unique<NiceMock<MockGLESImpl>>();
    detector = std::make_unique<FeedbackLoopDetector>(*implementation);
    coverage_detector = detector.get();
    ON_CALL(*implementation, GetIntegerv(_, _))
        .WillByDefault([](GLenum name, GLint* value) {
          *value =
              name == GL_MAX_TEXTURE_SIZE || name == GL_MAX_RENDERBUFFER_SIZE
                  ? 4096
              : name == GL_MAX_SAMPLES ? 4
              : name == GL_MAX_TEXTURE_IMAGE_UNITS ||
                      name == GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS
                  ? 16
                  : 0;
        });
    mock = MockGLES::Init(std::move(implementation), std::nullopt,
                          "OpenGL ES 3.0");
    AvioAntialiasingConfig policy;
    policy.policy = AvioAntialiasingPolicy::kCoverage;
    policy.layer_sample_count = 1;
    policy.coverage_sample_count = 4;
    policy.coverage_region_max_bytes = 8u * 1024u * 1024u;
    policy.layer_region_max_bytes = 4u * 1024u * 1024u;
    context = ContextGLES::Create(
        Flags{}, std::make_unique<ProcTableGLES>(CoverageResolver, true),
        ShaderLibraryMappings(), false, nullptr, policy);
    ASSERT_TRUE(context);
    worker = std::make_shared<MockReactorWorker>();
    ASSERT_TRUE(context->AddReactorWorker(worker));
    ASSERT_TRUE(std::static_pointer_cast<Context>(context)->IsValid());
    renderer = std::make_unique<ContentContext>(context,
                                                TypographerContextSkia::Make());
    ASSERT_TRUE(renderer->IsValid());
    ASSERT_TRUE(renderer->UsesAvioCoverage());
    ASSERT_FALSE(std::static_pointer_cast<Context>(context)
                     ->GetCapabilities()
                     ->SupportsFramebufferFetch());
    TextureDescriptor desc;
    // Match the persistent external target descriptor used by the embedder.
    desc.storage_mode = StorageMode::kDevicePrivate;
    desc.size = size;
    desc.format = PixelFormat::kR8G8B8A8UNormInt;
    desc.usage = TextureUsage::kRenderTarget;
    auto root = TextureGLES::WrapFBO(context->GetReactor(), desc, kRoot);
    ASSERT_TRUE(root);
    detector->RegisterBorrowedFBO(kRoot, kBorrowedTexture);
    ColorAttachment color;
    color.texture = root;
    color.load_action = LoadAction::kClear;
    color.store_action = StoreAction::kStore;
    color.clear_color = Color::Blue();
    ASSERT_TRUE(color.IsValid());
    target.SetColorAttachment(color, 0);
    ASSERT_EQ(target.GetRenderTargetTexture(), root);
    ASSERT_TRUE(target.IsValid());
  }

  void TearDown() override {
    if (worker) {
      worker->current = true;
    }
    if (context && worker) {
      EXPECT_TRUE(context->GetReactor()->React());
    }
    target = {};
    renderer.reset();
    context.reset();
    mock.reset();
    coverage_detector = nullptr;
  }

  std::unique_ptr<Canvas> NewCanvas() {
    return std::make_unique<Canvas>(*renderer, target, true, false,
                                    Rect::MakeSize(size));
  }

  // Endpoints include the wrapped FBO's Y reversal. A native island seed has
  // a different footprint, so this checks the actual backdrop capture copy.
  size_t CountCopies(IRect roi) const {
    const std::array<GLint, 8> endpoints = {
        static_cast<GLint>(roi.GetLeft()),
        static_cast<GLint>(size.height - roi.GetTop()),
        static_cast<GLint>(roi.GetRight()),
        static_cast<GLint>(size.height - roi.GetBottom()),
        0,
        0,
        static_cast<GLint>(roi.GetWidth()),
        static_cast<GLint>(roi.GetHeight())};
    size_t count = 0;
    for (const auto& copy : detector->RootBlits(kRoot)) {
      if (copy.coordinates == endpoints) {
        ++count;
        EXPECT_NE(copy.destination_texture, 0u);
        EXPECT_NE(copy.destination_texture, kBorrowedTexture);
        EXPECT_GT(copy.clear_serial, copy.prior_copy_serial);
        EXPECT_LT(copy.clear_serial, copy.serial);
      }
    }
    return count;
  }

  void ExpectNoFeedback() const {
    EXPECT_GT(detector->GetSamplingDrawCount(), 0u);
    EXPECT_THAT(detector->GetFeedbackLoopTextures(), IsEmpty());
  }

  std::unique_ptr<FeedbackLoopDetector> detector;
  std::shared_ptr<MockGLES> mock;
  std::shared_ptr<ContextGLES> context;
  std::shared_ptr<MockReactorWorker> worker;
  std::unique_ptr<ContentContext> renderer;
  RenderTarget target;
};
}  // namespace

TEST_F(CanvasCoverageFramebufferTest,
       UngroupedBlurCopiesItsHaloBeforeSampling) {
  auto canvas = NewCanvas();
  auto blur =
      flutter::DlImageFilter::MakeBlur(4, 4, flutter::DlTileMode::kClamp);
  canvas->SetBackdropData({}, 1);
  canvas->Save(32);
  canvas->ClipGeometry(FillRectGeometry(Rect::MakeXYWH(160, 140, 32, 20)),
                       Entity::ClipOperation::kIntersect, false);
  canvas->SaveLayer({}, Rect::MakeXYWH(160, 140, 32, 20), blur.get(),
                    ContentBoundsPromise::kContainsContents, 1);
  canvas->Restore();
  canvas->Restore();
  ASSERT_TRUE(canvas->EndReplay());
  EXPECT_EQ(CountCopies(IRect::MakeXYWH(147, 127, 58, 46)), 1u);
  ExpectNoFeedback();
}

TEST_F(CanvasCoverageFramebufferTest,
       GroupedBlurFreezesOneUnionBeforeBothCards) {
  auto canvas = NewCanvas();
  auto blur =
      flutter::DlImageFilter::MakeBlur(4, 4, flutter::DlTileMode::kClamp);
  BackdropData group;
  group.backdrop_count = 2;
  group.all_filters_equal = false;
  group.required_input_coverage = Rect::MakeXYWH(148, 128, 164, 44);
  canvas->SetBackdropData({{7, group}}, 2);
  for (Scalar x : {160.f, 268.f}) {
    canvas->Save(32);
    canvas->ClipGeometry(FillRectGeometry(Rect::MakeXYWH(x, 140, 32, 20)),
                         Entity::ClipOperation::kIntersect, false);
    canvas->SaveLayer({}, Rect::MakeXYWH(x, 140, 32, 20), blur.get(),
                      ContentBoundsPromise::kContainsContents, 1, false, 7);
    canvas->Restore();
    canvas->Restore();
    // Intervening content must not change the second card's frozen source.
    canvas->DrawRect(Rect::MakeXYWH(220, 145, 12, 8), {.color = Color::Red()});
  }
  ASSERT_TRUE(canvas->EndReplay());
  EXPECT_EQ(CountCopies(IRect::MakeXYWH(147, 127, 166, 46)), 1u);
  ExpectNoFeedback();
}

TEST_F(CanvasCoverageFramebufferTest, UngroupedLaterCardCapturesItsNewPrefix) {
  auto canvas = NewCanvas();
  auto blur =
      flutter::DlImageFilter::MakeBlur(4, 4, flutter::DlTileMode::kClamp);
  canvas->SetBackdropData({}, 2);
  for (int i = 0; i < 2; ++i) {
    canvas->Save(32);
    canvas->ClipGeometry(FillRectGeometry(Rect::MakeXYWH(160, 140, 32, 20)),
                         Entity::ClipOperation::kIntersect, false);
    canvas->SaveLayer({}, Rect::MakeXYWH(160, 140, 32, 20), blur.get(),
                      ContentBoundsPromise::kContainsContents, 1);
    canvas->Restore();
    canvas->Restore();
    canvas->DrawRect(Rect::MakeXYWH(160, 140, 12, 8), {.color = Color::Red()});
  }
  ASSERT_TRUE(canvas->EndReplay());
  EXPECT_EQ(CountCopies(IRect::MakeXYWH(147, 127, 58, 46)), 2u);
  ExpectNoFeedback();
}

TEST_F(CanvasCoverageFramebufferTest, OrdinaryAdvancedBlendCopiesDestination) {
  auto canvas = NewCanvas();
  canvas->DrawRect(Rect::MakeXYWH(160, 140, 32, 20),
                   {.color = Color::Orange(),
                    .blend_mode = BlendMode::kMultiply,
                    .anti_alias = false});
  ASSERT_TRUE(canvas->EndReplay());
  EXPECT_EQ(CountCopies(IRect::MakeXYWH(159, 139, 34, 22)), 1u);
  ExpectNoFeedback();
}

TEST_F(CanvasCoverageFramebufferTest, RestoredAdvancedBlendCopiesDestination) {
  auto canvas = NewCanvas();
  canvas->SaveLayer({.blend_mode = BlendMode::kMultiply},
                    Rect::MakeXYWH(160, 140, 32, 20), nullptr,
                    ContentBoundsPromise::kContainsContents, 1);
  canvas->DrawRect(Rect::MakeXYWH(160, 140, 32, 20),
                   {.color = Color::Orange(), .anti_alias = false});
  canvas->Restore();
  ASSERT_TRUE(canvas->EndReplay());
  EXPECT_EQ(CountCopies(IRect::MakeXYWH(159, 139, 34, 22)), 1u);
  ExpectNoFeedback();
}

TEST_F(CanvasCoverageFramebufferTest, RepeatAndMirrorPreserveWholeRootPeriod) {
  for (auto mode :
       {flutter::DlTileMode::kRepeat, flutter::DlTileMode::kMirror}) {
    auto canvas = NewCanvas();
    auto blur = flutter::DlImageFilter::MakeBlur(4, 4, mode);
    canvas->SetBackdropData({}, 1);
    canvas->Save(32);
    canvas->ClipGeometry(FillRectGeometry(Rect::MakeXYWH(1, 17, 32, 20)),
                         Entity::ClipOperation::kIntersect, false);
    canvas->SaveLayer({}, Rect::MakeXYWH(1, 17, 32, 20), blur.get(),
                      ContentBoundsPromise::kContainsContents, 1);
    canvas->Restore();
    canvas->Restore();
    ASSERT_TRUE(canvas->EndReplay());
  }
  EXPECT_EQ(CountCopies(IRect::MakeSize(size)), 2u);
  ExpectNoFeedback();
}

TEST_F(CanvasCoverageFramebufferTest,
       LaterGroupedNonlocalFilterRequiresFullInput) {
  auto clamp =
      flutter::DlImageFilter::MakeBlur(4, 4, flutter::DlTileMode::kClamp);
  auto repeat =
      flutter::DlImageFilter::MakeBlur(4, 4, flutter::DlTileMode::kRepeat);
  auto matrix = flutter::DlImageFilter::MakeMatrix(
      Matrix::MakeTranslation(Vector3(3, 7, 0)),
      flutter::DlImageSampling::kLinear);
  auto composed = flutter::DlImageFilter::MakeCompose(matrix, repeat);
  auto local =
      composed->makeWithLocalMatrix(Matrix::MakeScale(Vector3(2, 1, 1)));
  ASSERT_TRUE(local);
  EXPECT_FALSE(BackdropFilterRequiresFullInput(clamp.get()));
  EXPECT_TRUE(BackdropFilterRequiresFullInput(local.get()));
  FirstPassDispatcher first(*renderer, Matrix(), Rect::MakeSize(size));
  first.saveLayer(Rect::MakeXYWH(160, 140, 32, 20), {}, clamp.get(), 7);
  first.restore();
  first.saveLayer(Rect::MakeXYWH(1, 17, 32, 20), {}, local.get(), 7);
  first.restore();
  auto [groups, count] = first.TakeBackdropData();
  EXPECT_EQ(count, 2u);
  ASSERT_EQ(groups.size(), 1u);
  EXPECT_TRUE(groups.at(7).requires_full_backdrop);
}

TEST_F(CanvasCoverageFramebufferTest, TransformedHaloUsesGlobalDeviceBounds) {
  auto canvas = NewCanvas();
  auto blur =
      flutter::DlImageFilter::MakeBlur(4, 4, flutter::DlTileMode::kClamp);
  canvas->SetBackdropData({}, 1);
  canvas->Concat(Matrix::MakeTranslation(Vector3(30, 20, 0)) *
                 Matrix::MakeScale(Vector3(2, 1, 1)));
  canvas->Save(32);
  canvas->ClipGeometry(FillRectGeometry(Rect::MakeXYWH(40, 70, 20, 10)),
                       Entity::ClipOperation::kIntersect, false);
  canvas->SaveLayer({}, Rect::MakeXYWH(40, 70, 20, 10), blur.get(),
                    ContentBoundsPromise::kContainsContents, 1);
  canvas->Restore();
  canvas->Restore();
  ASSERT_TRUE(canvas->EndReplay());
  EXPECT_EQ(CountCopies(IRect::MakeXYWH(85, 77, 90, 36)), 1u);
  ExpectNoFeedback();
}

TEST_F(CanvasCoverageFramebufferTest, ClampHaloClipsAtAsymmetricRootEdge) {
  auto canvas = NewCanvas();
  auto blur =
      flutter::DlImageFilter::MakeBlur(4, 4, flutter::DlTileMode::kClamp);
  canvas->SetBackdropData({}, 1);
  canvas->Save(32);
  canvas->ClipGeometry(FillRectGeometry(Rect::MakeXYWH(1, 17, 32, 20)),
                       Entity::ClipOperation::kIntersect, false);
  canvas->SaveLayer({}, Rect::MakeXYWH(1, 17, 32, 20), blur.get(),
                    ContentBoundsPromise::kContainsContents, 1);
  canvas->Restore();
  canvas->Restore();
  ASSERT_TRUE(canvas->EndReplay());
  EXPECT_EQ(CountCopies(IRect::MakeXYWH(0, 4, 46, 46)), 1u);
  ExpectNoFeedback();
}

TEST_F(CanvasCoverageFramebufferTest, ExhaustedLayerClaimsFailCanvasReplay) {
  auto canvas = NewCanvas();
  std::vector<std::shared_ptr<AvioCoverageRegion::Lease>> owners;
  auto region = renderer->GetAvioCoverageRegion();
  // Fill only the predeclared bank claim bound; no test grows an unbounded
  // retry queue. Small 1x overflow allocations remain the existing policy.
  for (size_t i = 0; i < AvioCoverageRegion::kMaximumLeaseClaims; ++i) {
    auto admission = region->AcquireLayer({16, 16});
    if (!admission.lease) {
      EXPECT_EQ(admission.status, AvioCoverageRegion::Status::kNeedsFlush);
      break;
    }
    owners.push_back(std::move(admission.lease));
  }
  ASSERT_FALSE(owners.empty());
  ASSERT_FALSE(region->AcquireLayer({16, 16}).lease);
  auto blur =
      flutter::DlImageFilter::MakeBlur(4, 4, flutter::DlTileMode::kClamp);
  canvas->SetBackdropData({}, 1);
  canvas->Save(32);
  canvas->ClipGeometry(FillRectGeometry(Rect::MakeXYWH(160, 140, 32, 20)),
                       Entity::ClipOperation::kIntersect, false);
  canvas->SaveLayer({}, Rect::MakeXYWH(160, 140, 32, 20), blur.get(),
                    ContentBoundsPromise::kContainsContents, 1);
  canvas->Restore();
  canvas->Restore();
  EXPECT_FALSE(canvas->EndReplay());
  EXPECT_EQ(CountCopies(IRect::MakeSize(size)), 0u);
  EXPECT_EQ(CountCopies(IRect::MakeXYWH(147, 127, 58, 46)), 0u);
}

TEST_F(CanvasCoverageFramebufferTest, GreeterSigma30CopiesRequiredInputOnly) {
  auto canvas = NewCanvas();
  auto blur =
      flutter::DlImageFilter::MakeBlur(30, 30, flutter::DlTileMode::kClamp);
  canvas->SetBackdropData({}, 1);
  canvas->Save(32);
  canvas->ClipGeometry(FillRectGeometry(Rect::MakeXYWH(200, 150, 40, 20)),
                       Entity::ClipOperation::kIntersect, false);
  canvas->SaveLayer({}, Rect::MakeXYWH(200, 150, 40, 20), blur.get(),
                    ContentBoundsPromise::kContainsContents, 1);
  canvas->Restore();
  canvas->Restore();
  ASSERT_TRUE(canvas->EndReplay());
  EXPECT_EQ(CountCopies(IRect::MakeXYWH(109, 59, 222, 202)), 1u);
  EXPECT_EQ(CountCopies(IRect::MakeSize(size)), 0u);
  ExpectNoFeedback();
}

TEST_F(CanvasCoverageFramebufferTest,
       EqualGroupedFiltersReuseFrozenFilteredPrefix) {
  auto canvas = NewCanvas();
  auto blur =
      flutter::DlImageFilter::MakeBlur(4, 4, flutter::DlTileMode::kClamp);
  BackdropData group;
  group.backdrop_count = 2;
  group.all_filters_equal = true;
  group.required_input_coverage = Rect::MakeXYWH(148, 128, 164, 44);
  canvas->SetBackdropData({{7, group}}, 2);
  for (Scalar x : {160.f, 268.f}) {
    canvas->Save(32);
    canvas->ClipGeometry(FillRectGeometry(Rect::MakeXYWH(x, 140, 32, 20)),
                         Entity::ClipOperation::kIntersect, false);
    canvas->SaveLayer({}, Rect::MakeXYWH(x, 140, 32, 20), blur.get(),
                      ContentBoundsPromise::kContainsContents, 1, false, 7);
    canvas->Restore();
    canvas->Restore();
  }
  ASSERT_TRUE(canvas->EndReplay());
  EXPECT_EQ(CountCopies(IRect::MakeXYWH(147, 127, 166, 46)), 1u);
  ExpectNoFeedback();
}

TEST_F(CanvasCoverageFramebufferTest, TransformedAdvancedBlendUsesTargetROI) {
  auto canvas = NewCanvas();
  canvas->Concat(Matrix::MakeTranslation(Vector3(30, 20, 0)) *
                 Matrix::MakeScale(Vector3(2, 1, 1)));
  canvas->DrawRect(Rect::MakeXYWH(40, 70, 20, 10),
                   {.color = Color::Orange(),
                    .blend_mode = BlendMode::kMultiply,
                    .anti_alias = false});
  ASSERT_TRUE(canvas->EndReplay());
  EXPECT_EQ(CountCopies(IRect::MakeXYWH(109, 89, 42, 12)), 1u);
  ExpectNoFeedback();
}

TEST_F(CanvasCoverageFramebufferTest, PreservedRootCaptureDoesNotClearRoot) {
  auto color = target.GetColorAttachment(0);
  color.load_action = LoadAction::kLoad;
  target.SetColorAttachment(color, 0);
  auto canvas = NewCanvas();
  auto blur =
      flutter::DlImageFilter::MakeBlur(4, 4, flutter::DlTileMode::kClamp);
  canvas->SetBackdropData({}, 1);
  canvas->Save(32);
  canvas->ClipGeometry(FillRectGeometry(Rect::MakeXYWH(160, 140, 32, 20)),
                       Entity::ClipOperation::kIntersect, false);
  canvas->SaveLayer({}, Rect::MakeXYWH(160, 140, 32, 20), blur.get(),
                    ContentBoundsPromise::kContainsContents, 1);
  canvas->Restore();
  canvas->Restore();
  ASSERT_TRUE(canvas->EndReplay());
  EXPECT_EQ(CountCopies(IRect::MakeXYWH(147, 127, 58, 46)), 1u);
  EXPECT_EQ(detector->GetColorClearCount(kBorrowedTexture), 0u);
  ExpectNoFeedback();
}

TEST_F(CanvasCoverageFramebufferTest,
       CaptureSeparatesPhysicalAndLogicalOrigins) {
  ASSERT_TRUE(renderer->BeginAvioRasterFrame());
  fml::ScopedCleanupClosure end_frame([&] { renderer->EndAvioRasterFrame(); });
  ASSERT_TRUE(target.SetContentRect(IRect::MakeXYWH(11, 17, 400, 240)));
  auto source_owner = std::make_shared<int>(37);
  std::weak_ptr<int> source_observer = source_owner;
  target.SetResourceOwner(source_owner);
  const auto required = Rect::MakeXYWH(52, 63, 20, 10);
  const auto opaque = Rect::MakeXYWH(55, 65, 8, 5);
  auto snapshot = canvas_internal::CaptureCoverageBackdrop(*renderer, target,
                                                           required, opaque);
  ASSERT_TRUE(snapshot);
  // The logical ROI expands by one pixel; physical read coordinates alone
  // include source content (11,17). Snapshot space must remain logical.
  EXPECT_EQ(CountCopies(IRect::MakeXYWH(62, 79, 22, 12)), 1u);
  ASSERT_TRUE(snapshot->GetCoverage());
  EXPECT_EQ(*snapshot->GetCoverage(), Rect::MakeXYWH(51, 62, 22, 12));
  const auto content = snapshot->GetTextureRect();
  EXPECT_EQ(content.GetSize(), Size(22, 12));
  EXPECT_EQ(snapshot->transform * content.GetOrigin(), Point(51, 62));
  ASSERT_TRUE(snapshot->resource_owner);
  ASSERT_TRUE(snapshot->GetCapturedOpaqueRect());
  EXPECT_EQ(
      snapshot->GetCapturedOpaqueRect()->TransformBounds(snapshot->transform),
      opaque);
  EXPECT_TRUE(snapshot->IsImmutableCapturedBackdrop());
  target.SetResourceOwner(nullptr);
  source_owner.reset();
  EXPECT_TRUE(source_observer.expired());  // The immediate copy consumed it.
}

TEST_F(CanvasCoverageFramebufferTest, ReusedWarmImageGetsANewFullClear) {
  ASSERT_TRUE(renderer->BeginAvioRasterFrame());
  fml::ScopedCleanupClosure end_frame([&] { renderer->EndAvioRasterFrame(); });
  auto first = canvas_internal::CaptureCoverageBackdrop(
      *renderer, target, Rect::MakeXYWH(52, 63, 20, 10), std::nullopt);
  ASSERT_TRUE(first);
  EXPECT_GT(first->texture->GetSize().Area(), first->GetTextureRect().Area());
  ASSERT_EQ(CountCopies(IRect::MakeXYWH(51, 62, 22, 12)), 1u);
  auto copies = detector->RootBlits(kRoot);
  ASSERT_EQ(copies.size(), 1u);
  const auto first_copy = copies.front();
  const auto first_clear_count =
      detector->GetColorClearCount(first_copy.destination_texture);
  first.reset();
  auto second = canvas_internal::CaptureCoverageBackdrop(
      *renderer, target, Rect::MakeXYWH(152, 163, 20, 10), std::nullopt);
  ASSERT_TRUE(second);
  ASSERT_EQ(CountCopies(IRect::MakeXYWH(151, 162, 22, 12)), 1u);
  copies = detector->RootBlits(kRoot);
  ASSERT_EQ(copies.size(), 2u);
  EXPECT_EQ(copies.back().destination_texture, first_copy.destination_texture);
  EXPECT_GT(copies.back().clear_serial, first_copy.serial);
  EXPECT_EQ(detector->GetColorClearCount(first_copy.destination_texture),
            first_clear_count + 1u);
}

TEST_F(CanvasCoverageFramebufferTest, BlurUVsUseEvaluatedSnapshotTexelsOnce) {
  // A cropped root's scene origin survives exact-extent normalization, but
  // must not offset the physical texels sampled by the downsample pass.
  for (const auto size : {ISize(40, 32), ISize(256, 256)}) {
    auto descriptor = target.GetRenderTargetTexture()->GetTextureDescriptor();
    descriptor.size = size;
    auto texture =
        TextureGLES::WrapFBO(context->GetReactor(), descriptor, kRoot);
    ASSERT_TRUE(texture);
    Snapshot snapshot;
    snapshot.texture = texture;
    snapshot.texture_rect = Rect::MakeSize(ISize(40, 32));
    snapshot.transform = Matrix::MakeTranslation(Vector3(4, 7, 0));
    const auto uvs = GaussianBlurFilterContents::CalculateUVs(
        snapshot, Rect::MakeXYWH(-6, -6, 52, 44));
    EXPECT_NEAR(uvs[0].x, -6.f / size.width, 1e-6f);
    EXPECT_NEAR(uvs[0].y, -6.f / size.height, 1e-6f);
    EXPECT_NEAR(uvs[1].x, 46.f / size.width, 1e-6f);
    EXPECT_NEAR(uvs[1].y, -6.f / size.height, 1e-6f);
    EXPECT_NEAR(uvs[2].x, -6.f / size.width, 1e-6f);
    EXPECT_NEAR(uvs[2].y, 38.f / size.height, 1e-6f);
    EXPECT_NEAR(uvs[3].x, 46.f / size.width, 1e-6f);
    EXPECT_NEAR(uvs[3].y, 38.f / size.height, 1e-6f);
  }
}

TEST(GaussianBlurTextureCopyUniforms, ClearsStaleSamplingAndCoverageModes) {
  TextureFillFragmentShader::FragInfo uniforms = {};
  uniforms.image_sample_rect = Vector4(1, 2, 3, 4);
  uniforms.image_edge_rect = Vector4(5, 6, 7, 8);
  uniforms.continuous_image_edge = 1e30f;
  uniforms.analytic_quad = 2.f;
  uniforms.alpha = .125f;
  uniforms.external_linear_backdrop = 1.f;

  gaussian_blur_internal::InitializeTextureCopyUniforms(uniforms);

  EXPECT_EQ(uniforms.image_sample_rect, Vector4());
  EXPECT_EQ(uniforms.image_edge_rect, Vector4());
  EXPECT_EQ(uniforms.continuous_image_edge, 0.f);
  EXPECT_EQ(uniforms.analytic_quad, 0.f);
  EXPECT_EQ(uniforms.external_linear_backdrop, 0.f);
  EXPECT_EQ(uniforms.alpha, 1.f);
}

TEST(GaussianBlurTextureCopyUniforms, ClearsNonfinitePriorPacketValues) {
  TextureFillFragmentShader::FragInfo uniforms = {};
  const Scalar poison = std::numeric_limits<Scalar>::quiet_NaN();
  uniforms.image_sample_rect = Vector4(poison, poison, poison, poison);
  uniforms.image_edge_rect = Vector4(poison, poison, poison, poison);
  uniforms.continuous_image_edge = poison;
  uniforms.analytic_quad = poison;
  uniforms.alpha = poison;
  uniforms.external_linear_backdrop = poison;

  gaussian_blur_internal::InitializeTextureCopyUniforms(uniforms);

  EXPECT_EQ(uniforms.image_sample_rect, Vector4());
  EXPECT_EQ(uniforms.image_edge_rect, Vector4());
  EXPECT_EQ(uniforms.continuous_image_edge, 0.f);
  EXPECT_EQ(uniforms.analytic_quad, 0.f);
  EXPECT_EQ(uniforms.external_linear_backdrop, 0.f);
  EXPECT_EQ(uniforms.alpha, 1.f);
}

}  // namespace testing
}  // namespace impeller
