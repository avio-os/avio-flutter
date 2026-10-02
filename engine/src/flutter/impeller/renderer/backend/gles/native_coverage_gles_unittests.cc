// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/gles/native_coverage_gles.h"

#include <cstring>
#include <string_view>

#include "gtest/gtest.h"

namespace impeller {
namespace testing {
namespace {
struct ProbeState {
  int max_samples = 4;
  int samples = 4;
  int root_samples = 0, root_depth = 0, root_stencil = 0;
  int read = 31, draw = 32, renderbuffer = 33, texture = 34;
  unsigned error = 0;
  unsigned next = 100;
  int created = 0, deleted = 0, resolves = 0;
  bool incomplete = false, resolve_error = false, scissor = true;
};
thread_local ProbeState* state;
unsigned GetError() {
  auto error = state->error;
  state->error = 0;
  return error;
}
void GetIntegerv(unsigned key, int* value) {
  switch (key) {
    case 0x8D57:
      *value = state->max_samples;
      break;
    case 0x0D33:
    case 0x84E8:
      *value = 4096;
      break;
    case 0x8CAA:
      *value = state->read;
      break;
    case 0x8CA6:
      *value = state->draw;
      break;
    case 0x8CA7:
      *value = state->renderbuffer;
      break;
    case 0x8069:
      *value = state->texture;
      break;
    case 0x80A9:
      *value = state->root_samples;
      break;
    case 0x0D56:
      *value = state->root_depth;
      break;
    case 0x0D57:
      *value = state->root_stencil;
      break;
    default:
      ADD_FAILURE() << key;
      break;
  }
}
void Gen(int count, unsigned* objects) {
  for (int i = 0; i < count; ++i) {
    objects[i] = ++state->next;
    state->created++;
  }
}
void Delete(int count, const unsigned* objects) {
  for (int i = 0; i < count; ++i) {
    if (objects[i])
      state->deleted++;
  }
}
void BindFramebuffer(unsigned target, unsigned object) {
  if (target == 0x8CA8 || target == 0x8D40)
    state->read = object;
  if (target == 0x8CA9 || target == 0x8D40)
    state->draw = object;
}
unsigned CheckFramebufferStatus(unsigned) {
  return state->incomplete ? 0x8CD6 : 0x8CD5;
}
void BindRenderbuffer(unsigned, unsigned object) {
  state->renderbuffer = object;
}
void Storage(unsigned, int samples, unsigned format, int width, int height) {
  EXPECT_EQ(samples, 4);
  EXPECT_EQ(width, 8);
  EXPECT_EQ(height, 8);
  EXPECT_TRUE(format == 0x8058 || format == 0x88F0);
}
void GetRenderbufferParameteriv(unsigned, unsigned key, int* value) {
  EXPECT_EQ(key, 0x8CABu);
  *value = state->samples;
}
void FramebufferRenderbuffer(unsigned, unsigned, unsigned, unsigned) {}
void BindTexture(unsigned, unsigned object) {
  state->texture = object;
}
void TexImage2D(unsigned,
                int,
                int format,
                int,
                int,
                int,
                unsigned,
                unsigned,
                const void*) {
  EXPECT_EQ(format, 0x8058);
}
void FramebufferTexture2D(unsigned, unsigned, unsigned, unsigned, int) {}
void BlitFramebuffer(int,
                     int,
                     int,
                     int,
                     int,
                     int,
                     int,
                     int,
                     unsigned,
                     unsigned) {
  EXPECT_FALSE(state->scissor);
  state->resolves++;
  if (state->resolve_error)
    state->error = 0x0502;
}
unsigned char IsEnabled(unsigned) {
  return state->scissor;
}
void Disable(unsigned) {
  state->scissor = false;
}
void Enable(unsigned) {
  state->scissor = true;
}
void* Resolve(const char* name) {
#define PROC(name_, fn)                    \
  if (std::strcmp(name, "gl" #name_) == 0) \
  return reinterpret_cast<void*>(fn)
  PROC(GetError, GetError);
  PROC(GetIntegerv, GetIntegerv);
  PROC(GenFramebuffers, Gen);
  PROC(DeleteFramebuffers, Delete);
  PROC(BindFramebuffer, BindFramebuffer);
  PROC(CheckFramebufferStatus, CheckFramebufferStatus);
  PROC(GenRenderbuffers, Gen);
  PROC(DeleteRenderbuffers, Delete);
  PROC(BindRenderbuffer, BindRenderbuffer);
  PROC(RenderbufferStorageMultisample, Storage);
  PROC(GetRenderbufferParameteriv, GetRenderbufferParameteriv);
  PROC(FramebufferRenderbuffer, FramebufferRenderbuffer);
  PROC(GenTextures, Gen);
  PROC(DeleteTextures, Delete);
  PROC(BindTexture, BindTexture);
  PROC(TexImage2D, TexImage2D);
  PROC(FramebufferTexture2D, FramebufferTexture2D);
  PROC(BlitFramebuffer, BlitFramebuffer);
  PROC(IsEnabled, IsEnabled);
  PROC(Disable, Disable);
  PROC(Enable, Enable);
#undef PROC
  return nullptr;
}
void ExpectRestored(const ProbeState& probe) {
  EXPECT_EQ(probe.read, 31);
  EXPECT_EQ(probe.draw, 32);
  EXPECT_EQ(probe.renderbuffer, 33);
  EXPECT_EQ(probe.texture, 34);
  EXPECT_TRUE(probe.scissor);
  EXPECT_EQ(probe.created, probe.deleted);
}
}  // namespace

TEST(NativeCoverageGLES, LegacyAndMissingProcDoNotAllocateOrChangeState) {
  ProbeState probe;
  state = &probe;
  EXPECT_FALSE(ProbeNativeCoverageGLES(Resolve, false));
  EXPECT_FALSE(ProbeNativeCoverageGLES(
      [](const char* name) {
        return std::string_view(name) == "glBlitFramebuffer" ? nullptr
                                                             : Resolve(name);
      },
      true));
  EXPECT_EQ(probe.created, 0);
  ExpectRestored(probe);
}
TEST(NativeCoverageGLES, ExactNative4AndExplicitTextureResolveAreRequired) {
  ProbeState probe;
  state = &probe;
  EXPECT_TRUE(ProbeNativeCoverageGLES(Resolve, true));
  EXPECT_EQ(probe.resolves, 1);
  EXPECT_EQ(probe.created, 5);
  ExpectRestored(probe);
}
TEST(NativeCoverageGLES, AdvertisedMaximumDoesNotProveActualFormatSamples) {
  ProbeState probe;
  state = &probe;
  probe.samples = 2;
  EXPECT_FALSE(ProbeNativeCoverageGLES(Resolve, true));
  EXPECT_EQ(probe.resolves, 0);
  ExpectRestored(probe);
}
TEST(NativeCoverageGLES,
     IncompleteFramebufferAndFailedResolveRestoreAndDelete) {
  ProbeState probe;
  state = &probe;
  probe.incomplete = true;
  EXPECT_FALSE(ProbeNativeCoverageGLES(Resolve, true));
  ExpectRestored(probe);
  probe.incomplete = false;
  probe.resolve_error = true;
  EXPECT_FALSE(ProbeNativeCoverageGLES(Resolve, true));
  ExpectRestored(probe);
}
TEST(NativeCoverageGLES, UnsupportedLimitsRefuseBeforeAllocating) {
  ProbeState probe;
  state = &probe;
  probe.max_samples = 2;
  EXPECT_FALSE(ProbeNativeCoverageGLES(Resolve, true));
  EXPECT_EQ(probe.created, 0);
  ExpectRestored(probe);
}
TEST(NativeCoverageGLES, HostFramebufferMustBeCompleteSingleSampleColorOnly) {
  struct GL {
    void GetIntegerv(unsigned key, int* value) const {
      testing::GetIntegerv(key, value);
    }
    void BindFramebuffer(unsigned target, unsigned name) const {
      testing::BindFramebuffer(target, name);
    }
    unsigned CheckFramebufferStatus(unsigned target) const {
      return testing::CheckFramebufferStatus(target);
    }
  } gl;
  ProbeState probe;
  state = &probe;
  EXPECT_TRUE(ValidateCoverageFramebufferGLES(gl, 81));
  ExpectRestored(probe);
  probe.root_samples = 4;
  EXPECT_FALSE(ValidateCoverageFramebufferGLES(gl, 81));
  ExpectRestored(probe);
  probe.root_samples = 0;
  probe.root_depth = 24;
  EXPECT_FALSE(ValidateCoverageFramebufferGLES(gl, 81));
  ExpectRestored(probe);
  probe.root_depth = 0;
  probe.root_stencil = 8;
  EXPECT_FALSE(ValidateCoverageFramebufferGLES(gl, 81));
  ExpectRestored(probe);
  probe.root_stencil = 0;
  probe.incomplete = true;
  EXPECT_FALSE(ValidateCoverageFramebufferGLES(gl, 81));
  ExpectRestored(probe);
  EXPECT_EQ(probe.created, 0);
}
}  // namespace testing
}  // namespace impeller
