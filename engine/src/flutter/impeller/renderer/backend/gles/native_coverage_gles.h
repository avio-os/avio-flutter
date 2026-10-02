// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_NATIVE_COVERAGE_GLES_H_
#define FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_NATIVE_COVERAGE_GLES_H_

// Header-only so the GTK embedder and renderer use the same cold admission
// probe without linking GTK against private renderer symbols. GL/GLES 3.0 is
// required by the caller. The GL ABI's integer types/enums are common to both
// APIs; no competing GLES/epoxy headers are included here.
#ifdef _WIN32
#define AVIO_GL_CALL __stdcall
#else
#define AVIO_GL_CALL
#endif

namespace impeller {

// The host's actual framebuffer must obey the single-sample, color-only root
// contract. The caller holds a current context; this neither allocates nor
// changes attachment ownership, and restores independent read/draw bindings.
template <class GL>
bool ValidateCoverageFramebufferGLES(const GL& gl, unsigned framebuffer) {
  constexpr unsigned kRead = 0x8CA8, kDraw = 0x8CA9, kBoth = 0x8D40;
  int old_read = 0, old_draw = 0;
  gl.GetIntegerv(0x8CAA, &old_read);
  gl.GetIntegerv(0x8CA6, &old_draw);
  gl.BindFramebuffer(kBoth, framebuffer);
  int samples = -1, depth_bits = -1, stencil_bits = -1;
  gl.GetIntegerv(0x80A9, &samples);       // GL_SAMPLES: zero for single-sample.
  gl.GetIntegerv(0x0D56, &depth_bits);    // GL_DEPTH_BITS
  gl.GetIntegerv(0x0D57, &stencil_bits);  // GL_STENCIL_BITS
  const bool valid = gl.CheckFramebufferStatus(kBoth) == 0x8CD5 &&
                     samples == 0 && depth_bits == 0 && stencil_bits == 0;
  gl.BindFramebuffer(kRead, static_cast<unsigned>(old_read));
  gl.BindFramebuffer(kDraw, static_cast<unsigned>(old_draw));
  return valid;
}

template <class Resolver>
bool ProbeNativeCoverageGLES(const Resolver& resolver, bool supports_gl3) {
  if (!supports_gl3) {
    return false;
  }
#define AVIO_GL_PROC(name, result, ...)                             \
  auto name = reinterpret_cast<result(AVIO_GL_CALL*)(__VA_ARGS__)>( \
      resolver("gl" #name));                                        \
  if (!name) {                                                      \
    return false;                                                   \
  }
  AVIO_GL_PROC(GetError, unsigned);
  AVIO_GL_PROC(GetIntegerv, void, unsigned, int*);
  AVIO_GL_PROC(GenFramebuffers, void, int, unsigned*);
  AVIO_GL_PROC(DeleteFramebuffers, void, int, const unsigned*);
  AVIO_GL_PROC(BindFramebuffer, void, unsigned, unsigned);
  AVIO_GL_PROC(CheckFramebufferStatus, unsigned, unsigned);
  AVIO_GL_PROC(GenRenderbuffers, void, int, unsigned*);
  AVIO_GL_PROC(DeleteRenderbuffers, void, int, const unsigned*);
  AVIO_GL_PROC(BindRenderbuffer, void, unsigned, unsigned);
  AVIO_GL_PROC(RenderbufferStorageMultisample, void, unsigned, int, unsigned,
               int, int);
  AVIO_GL_PROC(GetRenderbufferParameteriv, void, unsigned, unsigned, int*);
  AVIO_GL_PROC(FramebufferRenderbuffer, void, unsigned, unsigned, unsigned,
               unsigned);
  AVIO_GL_PROC(GenTextures, void, int, unsigned*);
  AVIO_GL_PROC(DeleteTextures, void, int, const unsigned*);
  AVIO_GL_PROC(BindTexture, void, unsigned, unsigned);
  AVIO_GL_PROC(TexImage2D, void, unsigned, int, int, int, int, int, unsigned,
               unsigned, const void*);
  AVIO_GL_PROC(FramebufferTexture2D, void, unsigned, unsigned, unsigned,
               unsigned, int);
  AVIO_GL_PROC(BlitFramebuffer, void, int, int, int, int, int, int, int, int,
               unsigned, unsigned);
  AVIO_GL_PROC(IsEnabled, unsigned char, unsigned);
  AVIO_GL_PROC(Disable, void, unsigned);
  AVIO_GL_PROC(Enable, void, unsigned);
#undef AVIO_GL_PROC
  constexpr unsigned kRead = 0x8CA8, kDraw = 0x8CA9;
  constexpr unsigned kRenderbuffer = 0x8D41, kTexture2D = 0x0DE1;
  constexpr unsigned kColour = 0x8CE0, kDepth = 0x8D00, kStencil = 0x8D20;
  constexpr unsigned kComplete = 0x8CD5, kScissor = 0x0C11;
  if (GetError() != 0) {
    return false;
  }
  int max_samples = 0, max_texture = 0, max_renderbuffer = 0;
  GetIntegerv(0x8D57, &max_samples);       // GL_MAX_SAMPLES
  GetIntegerv(0x0D33, &max_texture);       // GL_MAX_TEXTURE_SIZE
  GetIntegerv(0x84E8, &max_renderbuffer);  // GL_MAX_RENDERBUFFER_SIZE
  if (GetError() != 0 || max_samples < 4 || max_texture < 768 ||
      max_renderbuffer < 256) {
    return false;
  }
  int old_read = 0, old_draw = 0, old_renderbuffer = 0, old_texture = 0;
  GetIntegerv(0x8CAA, &old_read);          // GL_READ_FRAMEBUFFER_BINDING
  GetIntegerv(0x8CA6, &old_draw);          // GL_DRAW_FRAMEBUFFER_BINDING
  GetIntegerv(0x8CA7, &old_renderbuffer);  // GL_RENDERBUFFER_BINDING
  GetIntegerv(0x8069, &old_texture);       // GL_TEXTURE_BINDING_2D
  const bool scissor = IsEnabled(kScissor) != 0;
  unsigned fbos[2] = {}, renderbuffers[2] = {}, texture = 0;
  GenFramebuffers(2, fbos);
  GenRenderbuffers(2, renderbuffers);
  GenTextures(1, &texture);
  bool supported = fbos[0] != 0 && fbos[1] != 0 && renderbuffers[0] != 0 &&
                   renderbuffers[1] != 0 && texture != 0;
  if (supported) {
    BindFramebuffer(kRead, fbos[0]);
    BindRenderbuffer(kRenderbuffer, renderbuffers[0]);
    RenderbufferStorageMultisample(kRenderbuffer, 4, 0x8058, 8, 8);  // RGBA8
    int colour_samples = 0;
    GetRenderbufferParameteriv(kRenderbuffer, 0x8CAB, &colour_samples);
    FramebufferRenderbuffer(kRead, kColour, kRenderbuffer, renderbuffers[0]);
    BindRenderbuffer(kRenderbuffer, renderbuffers[1]);
    RenderbufferStorageMultisample(kRenderbuffer, 4, 0x88F0, 8, 8);  // D24S8
    int stencil_samples = 0;
    GetRenderbufferParameteriv(kRenderbuffer, 0x8CAB, &stencil_samples);
    FramebufferRenderbuffer(kRead, kDepth, kRenderbuffer, renderbuffers[1]);
    FramebufferRenderbuffer(kRead, kStencil, kRenderbuffer, renderbuffers[1]);
    supported = colour_samples == 4 && stencil_samples == 4 &&
                CheckFramebufferStatus(kRead) == kComplete;
    BindTexture(kTexture2D, texture);
    TexImage2D(kTexture2D, 0, 0x8058, 8, 8, 0, 0x1908, 0x1401, nullptr);
    BindFramebuffer(kDraw, fbos[1]);
    FramebufferTexture2D(kDraw, kColour, kTexture2D, texture, 0);
    supported = supported && CheckFramebufferStatus(kDraw) == kComplete &&
                GetError() == 0;
    if (supported) {
      Disable(kScissor);
      // Resolve must be explicit. This also exercises the single-sample
      // texture-FBO path used by prefix scratch and tile copyback.
      BlitFramebuffer(0, 0, 8, 8, 0, 0, 8, 8, 0x4000, 0x2600);
      supported = GetError() == 0;
    }
  }
  BindFramebuffer(kRead, static_cast<unsigned>(old_read));
  BindFramebuffer(kDraw, static_cast<unsigned>(old_draw));
  BindRenderbuffer(kRenderbuffer, static_cast<unsigned>(old_renderbuffer));
  BindTexture(kTexture2D, static_cast<unsigned>(old_texture));
  if (scissor) {
    Enable(kScissor);
  }
  DeleteFramebuffers(2, fbos);
  DeleteRenderbuffers(2, renderbuffers);
  DeleteTextures(1, &texture);
  return GetError() == 0 && supported;
}

}  // namespace impeller
#undef AVIO_GL_CALL
#endif  // FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_NATIVE_COVERAGE_GLES_H_
