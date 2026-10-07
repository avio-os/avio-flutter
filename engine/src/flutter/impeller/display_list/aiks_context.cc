// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/display_list/aiks_context.h"
#include "impeller/display_list/coverage_classifier.h"
#include "impeller/entity/coverage_recorder_bank.h"

#include "impeller/typographer/typographer_context.h"

namespace impeller {

AiksContext::AiksContext(
    std::shared_ptr<Context> context,
    std::shared_ptr<TypographerContext> typographer_context,
    std::optional<std::shared_ptr<RenderTargetAllocator>>
        render_target_allocator)
    : context_(std::move(context)) {
  if (!context_ || !context_->IsValid()) {
    return;
  }

  content_context_ = std::make_unique<ContentContext>(
      context_, std::move(typographer_context),
      render_target_allocator.has_value() ? render_target_allocator.value()
                                          : nullptr);
  if (!content_context_->IsValid()) {
    return;
  }
  if (content_context_->UsesAvioCoverage()) {
    content_context_->SetCoverageClassifierStorage(
        std::make_shared<CoverageDisplayListPlan>(Rect{}, 4u));
  }

  is_valid_ = true;
}

AiksContext::~AiksContext() = default;

bool AiksContext::IsValid() const {
  return is_valid_;
}

std::shared_ptr<Context> AiksContext::GetContext() const {
  return context_;
}

ContentContext& AiksContext::GetContentContext() const {
  return *content_context_;
}

AvioRenderResourceReport AiksContext::GetAvioRenderResourceReport(
    bool start_new_interval) const {
  if (!content_context_) {
    return {};
  }
  auto report =
      content_context_->GetAvioRenderResourceReport(start_new_interval);
  if (!report.available) {
    return report;
  }
  CoverageFixedStorageUsage storage;
  if (const auto bank = content_context_->GetCoverageRecorderStorage()) {
    bank->AccumulateStorageUsage(storage);
  }
  if (const auto* plan = content_context_->GetCoverageClassifierPlan()) {
    plan->AccumulateStorageUsage(storage);
  }
  if (storage.capacity_bytes != 0u) {
    AvioRenderResourceEntry entry;
    entry.kind_id =
        static_cast<uint32_t>(AvioRenderResourceKind::kCoverageCpuStorage);
    entry.usage.entries = storage.high_water_elements;
    entry.usage.nominal_bytes = storage.capacity_bytes;
    entry.usage.real_bytes = storage.high_water_bytes;
    report.AddEntry(entry);
  }
  return report;
}

}  // namespace impeller
