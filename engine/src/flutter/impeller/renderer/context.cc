// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/context.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <utility>
#include "flutter/fml/trace_event.h"

namespace impeller {

ImpellerContextFuture::ImpellerContextFuture(
    std::future<std::shared_ptr<impeller::Context>> context)
    : future_(std::move(context)) {}

std::shared_ptr<impeller::Context> ImpellerContextFuture::GetContext() {
  std::scoped_lock<std::mutex> lock(mutex_);
  if (!did_wait_ && future_.valid()) {
    context_ = future_.get();
    did_wait_ = true;
  }
  return context_;
}

Context::~Context() = default;

Context::Context(const Flags& flags) : flags_(flags) {}

bool Context::UpdateOffscreenLayerPixelFormat(PixelFormat format) {
  return false;
}

bool Context::EnqueueCommandBuffer(
    std::shared_ptr<CommandBuffer> command_buffer) {
  return GetCommandQueue()->Submit({std::move(command_buffer)}).ok();
}

bool Context::FlushCommandBuffers() {
  return true;
}

std::shared_ptr<const IdleWaiter> Context::GetIdleWaiter() const {
  return nullptr;
}

std::shared_ptr<const GpuSubmissionTracker> Context::GetSubmissionTracker()
    const {
  return nullptr;
}

void Context::ResetThreadLocalState() const {
  // Nothing to do.
}

bool Context::AddTrackingFence(const std::shared_ptr<Texture>& texture) const {
  return false;
}

bool Context::SubmitOnscreen(std::shared_ptr<CommandBuffer> cmd_buffer) {
  return EnqueueCommandBuffer(std::move(cmd_buffer));
}

std::shared_ptr<AvioCoverageRegion> Context::GetAvioCoverageRegion() const {
  std::scoped_lock lock(avio_coverage_region_mutex_);
  return avio_coverage_region_;
}

void Context::RecordAvioCoverageDraw(AvioCoverageReason reason,
                                     uint64_t bounding_box_pixels) {
  RecordAvioCoverageClassification(reason, 1u, bounding_box_pixels);
}

void Context::RecordAvioCoverageClassification(AvioCoverageReason reason,
                                               uint64_t draw_count,
                                               uint64_t bounding_box_pixels) {
  if (!GetAvioAntialiasingConfig().UsesCoverage()) {
    return;
  }
  std::scoped_lock lock(avio_coverage_usage_mutex_);
  const auto id = static_cast<uint32_t>(reason);
  for (size_t i = 0; i < avio_coverage_usage_.coverage_reasons_count; ++i) {
    auto& entry = avio_coverage_usage_.coverage_reasons[i];
    if (entry.reason_id == id) {
      entry.draw_count += draw_count;
      entry.pixel_area += bounding_box_pixels;
      return;
    }
  }
  avio_coverage_usage_.AddCoverageReason({.reason_id = id,
                                          .draw_count = draw_count,
                                          .pixel_area = bounding_box_pixels});
}

AvioRenderResourceReport Context::GetAvioCoverageUsageReport(
    bool start_new_interval) const {
  std::scoped_lock lock(avio_coverage_usage_mutex_);
  auto result = avio_coverage_usage_;
  result.available = true;
  // Emit demand diagnostics on the requested report task, never during the
  // classifier's frame turn (timeline backends may own allocation machinery).
  char bytes[32], scopes[32];
  std::snprintf(bytes, sizeof(bytes), "%llu",
                static_cast<unsigned long long>(avio_nominal_layer_demand_));
  std::snprintf(scopes, sizeof(scopes), "%llu",
                static_cast<unsigned long long>(avio_classified_scope_count_));
  TRACE_EVENT_INSTANT2("impeller", "AvioCoverageClassification",
                       "peak_nominal_layer_bytes", bytes, "scope_count",
                       scopes);
  if (start_new_interval) {
    avio_coverage_usage_ = {};
    avio_nominal_layer_demand_ = 0;
    avio_classified_scope_count_ = 0;
  }
  return result;
}

void Context::RecordAvioCoverageLayerDemand(uint64_t bytes, uint64_t count) {
  std::scoped_lock lock(avio_coverage_usage_mutex_);
  avio_nominal_layer_demand_ = std::max(avio_nominal_layer_demand_, bytes);
  avio_classified_scope_count_ = std::max(avio_classified_scope_count_, count);
}

AvioRenderResourceReport Context::GetAvioCoverageRegionResourceReport(
    bool start_new_interval) const {
  std::lock_guard lock(avio_coverage_region_mutex_);
  // Providers read only the supplied region, and must not re-enter Context or
  // invoke embedder callbacks. No std::function copy/allocation occurs here.
  return avio_coverage_region_ && avio_region_descriptor_report_
             ? avio_region_descriptor_report_(*avio_coverage_region_,
                                              start_new_interval)
             : AvioRenderResourceReport{};
}

std::shared_ptr<AvioCoverageRegion> Context::InitializeAvioCoverageRegion(
    const std::function<std::shared_ptr<AvioCoverageRegion>()>& create,
    std::function<AvioRenderResourceReport(const AvioCoverageRegion&, bool)>
        descriptor_report) {
  std::scoped_lock lock(avio_coverage_region_mutex_);
  if (!avio_coverage_region_ && GetAvioAntialiasingConfig().UsesCoverage()) {
    avio_coverage_region_ = create();
    if (avio_coverage_region_) {
      avio_region_descriptor_report_ = std::move(descriptor_report);
    }
  }
  return avio_coverage_region_;
}

}  // namespace impeller
