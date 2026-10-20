// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/gles/context_gles.h"

#include <limits>

#include "flutter/fml/closure.h"
#include "impeller/renderer/backend/gles/native_coverage_gles.h"

namespace impeller {
namespace {
std::atomic_uint64_t next_external_target{1};

uint64_t AllocateTargetID() {
  auto next = next_external_target.load();
  while (next != std::numeric_limits<uint64_t>::max()) {
    if (next_external_target.compare_exchange_weak(next, next + 1)) {
      return next;
    }
  }
  return 0;
}

bool DeleteTarget(const ProcTableGLES& gl, GLuint framebuffer) {
  GLint read = 0, draw = 0;
  gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
  gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
  gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  // Detach before notifying the host that its EGLImage/texture can retire.
  gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                          0, 0);
  gl.DeleteFramebuffers(1, &framebuffer);
  gl.BindFramebuffer(GL_READ_FRAMEBUFFER,
                     static_cast<GLuint>(read) == framebuffer ? 0 : read);
  gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER,
                     static_cast<GLuint>(draw) == framebuffer ? 0 : draw);
  return gl.GetError() == GL_NO_ERROR;
}
}  // namespace

uint64_t ContextGLES::RegisterExternalTarget(GLuint texture, ISize size) {
  if (!texture || size.IsEmpty() || !IsValid() ||
      !antialiasing_config_.UsesCoverage() ||
      !reactor_->CanReactOnCurrentThread()) {
    return 0;
  }
  std::lock_guard lock(external_targets_mutex_);
  const auto thread = std::this_thread::get_id();
  if (external_target_owner_ && *external_target_owner_ != thread) {
    return 0;
  }
  external_target_owner_ = thread;
  const auto& gl = reactor_->GetProcTable();
  if (gl.GetError() != GL_NO_ERROR) {
    return 0;
  }
  const auto id = AllocateTargetID();
  if (id == 0) {
    MarkExternalCompletionUnobservable();
    return 0;
  }
  GLint read = 0, draw = 0;
  gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
  gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
  fml::ScopedCleanupClosure restore([&] {
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, read);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
  });
  GLuint framebuffer = 0;
  gl.GenFramebuffers(1, &framebuffer);
  if (!framebuffer) {
    return 0;
  }
  gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                          texture, 0);
  if (!ValidateCoverageFramebufferGLES(gl, framebuffer) ||
      gl.GetError() != GL_NO_ERROR) {
    if (!DeleteTarget(gl, framebuffer)) {
      MarkExternalCompletionUnobservable();
    }
    return 0;
  }
  restore.Reset();
  if (gl.GetError() != GL_NO_ERROR) {
    // Restoration itself can lose the context. Do not publish an admitted
    // binding or claim that this host image can be safely destroyed.
    MarkExternalCompletionUnobservable();
    return 0;
  }
  external_targets_.emplace(id, std::make_shared<ExternalTarget>(ExternalTarget{
                                    texture, framebuffer, size}));
  external_targets_created_++;
  return id;
}

std::shared_ptr<const ContextGLES::ExternalTarget>
ContextGLES::AcquireExternalTarget(uint64_t id,
                                   GLuint texture,
                                   ISize size) const {
  if (!IsValid() || !reactor_->CanReactOnCurrentThread()) {
    return nullptr;
  }
  std::lock_guard lock(external_targets_mutex_);
  if (external_target_owner_ != std::this_thread::get_id()) {
    return nullptr;
  }
  auto found = external_targets_.find(id);
  if (found == external_targets_.end() || found->second->texture != texture ||
      found->second->size != size) {
    return nullptr;
  }
  return found->second;
}

ContextGLES::ExternalTargetRetirement ContextGLES::RetireExternalTarget(
    uint64_t id) {
  if (!IsValid() || !reactor_->CanReactOnCurrentThread()) {
    return ExternalTargetRetirement::kFailed;
  }
  std::lock_guard lock(external_targets_mutex_);
  if (external_target_owner_ != std::this_thread::get_id()) {
    return ExternalTargetRetirement::kFailed;
  }
  auto found = external_targets_.find(id);
  if (found == external_targets_.end()) {
    return ExternalTargetRetirement::kUnknown;
  }
  if (found->second.use_count() != 1) {
    return ExternalTargetRetirement::kInUse;
  }
  const auto& gl = reactor_->GetProcTable();
  if (gl.GetError() != GL_NO_ERROR ||
      !DeleteTarget(gl, found->second->framebuffer)) {
    MarkExternalCompletionUnobservable();
    return ExternalTargetRetirement::kFailed;
  }
  external_targets_.erase(found);
  external_targets_retired_++;
  return ExternalTargetRetirement::kSuccess;
}

bool ContextGLES::RetireExternalRenderTargets() {
  for (;;) {
    uint64_t id;
    {
      std::lock_guard lock(external_targets_mutex_);
      if (external_targets_.empty()) {
        return true;
      }
      id = external_targets_.begin()->first;
    }
    if (RetireExternalTarget(id) != ExternalTargetRetirement::kSuccess) {
      MarkExternalCompletionUnobservable();
      return false;
    }
  }
}
}  // namespace impeller
