// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/command_buffer.h"

#include <algorithm>

#include "impeller/renderer/compute_pass.h"
#include "impeller/renderer/render_pass.h"
#include "impeller/renderer/render_target.h"

namespace impeller {

CommandBuffer::CommandBuffer(std::weak_ptr<const Context> context)
    : context_(std::move(context)) {}

CommandBuffer::~CommandBuffer() = default;

void CommandBuffer::RetainResource(std::shared_ptr<void> owner) {
  TryRetainResource(std::move(owner));
}

bool CommandBuffer::TryRetainResource(std::shared_ptr<void> owner) {
  if (!resource_owners_valid_) {
    return false;
  }
  return resource_owners_valid_ = retained_resources_.Retain(std::move(owner));
}

bool CommandBuffer::SubmitCommands(bool block_on_schedule,
                                   const CompletionCallback& callback) {
  if (!IsValid() || !HasValidResourceOwners()) {
    // Already committed or was never valid. Either way, this is caller error.
    if (callback) {
      callback(Status::kError);
    }
    return false;
  }
  return OnSubmitCommands(block_on_schedule, callback);
}

bool CommandBuffer::SubmitCommands(bool block_on_schedule) {
  return SubmitCommands(block_on_schedule, nullptr);
}

void CommandBuffer::WaitUntilCompleted() {
  return OnWaitUntilCompleted();
}

void CommandBuffer::WaitUntilScheduled() {
  return OnWaitUntilScheduled();
}

std::shared_ptr<RenderPass> CommandBuffer::CreateRenderPass(
    const RenderTarget& render_target) {
  if (!IsValid() || !HasValidResourceOwners()) {
    return nullptr;
  }
  auto pass = OnCreateRenderPass(render_target);
  if (pass && pass->IsValid()) {
    pass->SetLabel("RenderPass");
    return pass;
  }
  return nullptr;
}

std::shared_ptr<BlitPass> CommandBuffer::CreateBlitPass() {
  if (!IsValid() || !HasValidResourceOwners()) {
    return nullptr;
  }
  auto pass = OnCreateBlitPass();
  if (pass && pass->IsValid()) {
    pass->SetLabel("BlitPass");
    return pass;
  }
  return nullptr;
}

std::shared_ptr<ComputePass> CommandBuffer::CreateComputePass() {
  if (!IsValid() || !HasValidResourceOwners()) {
    return nullptr;
  }
  auto pass = OnCreateComputePass();
  if (pass && pass->IsValid()) {
    pass->SetLabel("ComputePass");
    return pass;
  }
  return nullptr;
}

}  // namespace impeller
