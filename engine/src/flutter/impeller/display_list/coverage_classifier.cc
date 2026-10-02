// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/display_list/coverage_classifier.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

#include "impeller/renderer/context.h"

namespace impeller {
namespace {
std::atomic<uint64_t> next_segment_token{1};
uint64_t Pixels(Rect rect) {
  if (!rect.IsFinite() || rect.IsEmpty())
    return 0;
  const auto size = IRect::RoundOut(rect).GetSize();
  return static_cast<uint64_t>(size.width) * size.height;
}
uint64_t Add(uint64_t a, uint64_t b) {
  return a > std::numeric_limits<uint64_t>::max() - b
             ? std::numeric_limits<uint64_t>::max()
             : a + b;
}
uint64_t Multiply(uint64_t a, uint64_t b) {
  return b && a > std::numeric_limits<uint64_t>::max() / b
             ? std::numeric_limits<uint64_t>::max()
             : a * b;
}
bool FringeTouches(const CoverageClipDecision& clip, Rect bounds) {
  if (clip.is_rect && clip.operation == ClipOperation::kIntersect) {
    const auto fringe = ComputeRectClipFringe4(clip.physical_bounds);
    if (fringe) {
      for (const auto& strip : fringe->fringe_pixels) {
        if (!Rect::MakeLTRB(strip.GetLeft(), strip.GetTop(), strip.GetRight(),
                            strip.GetBottom())
                 .IntersectionOrEmpty(bounds)
                 .IsEmpty())
          return true;
      }
      return false;
    }
  }
  return !clip.physical_bounds.IntersectionOrEmpty(bounds).IsEmpty();
}
}  // namespace

bool CanRenderLegacySdfAt1x(const CoverageDrawFacts& facts,
                            const Matrix& transform,
                            bool owner_use_sdfs) {
  return owner_use_sdfs &&
         (facts.kind == CoverageGeometryKind::kRect ||
          facts.kind == CoverageGeometryKind::kRoundRect) &&
         facts.anti_alias && !facts.stroke && facts.uniform_source &&
         facts.analytic_source_supported && !facts.reads_destination &&
         facts.blend == BlendMode::kSrcOver && transform.IsFinite() &&
         transform.IsAffine() && transform.IsInvertible() &&
         transform.Invert().IsFinite() && facts.physical_bounds.IsFinite();
}

CoverageDisplayListPlan::CoverageDisplayListPlan(Rect bounds, size_t bpp)
    : physical_bounds_(bounds), bytes_per_pixel_(bpp) {
  Reset(bounds, bpp);
}
void CoverageDisplayListPlan::Reset(Rect bounds, size_t bpp) {
  scopes_.clear();
  clips_.clear();
  active_clips_.clear();
  clip_stack_.clear();
  draw_records_.clear();
  images_.clear();
  saved_.clear();
  reasons_ = {};
  physical_bounds_ = bounds;
  bytes_per_pixel_ = bpp;
  prior_fringe_bounds_ = {};
  current_scope_ = 0;
  live_layer_bytes_ = 0;
  live_layer_pixels_ = 0;
  peak_live_layer_bytes_ = 0;
  peak_live_layer_pixels_ = 0;
  overflow_ = false;
  finalized_ = false;
  scopes_.push_back({.physical_bounds = bounds});
}
void CoverageDisplayListPlan::Invalidate() {
  overflow_ = true;
  for (auto& scope : scopes_)
    scope.can_render_direct_1x = false;
}
Rect CoverageDisplayListPlan::Bounded(Rect bounds) const {
  return bounds.IsFinite() ? bounds.IntersectionOrEmpty(physical_bounds_)
                           : physical_bounds_;
}
void CoverageDisplayListPlan::Save() {
  if (overflow_)
    return;
  if (!saved_.push_back({current_scope_, active_clips_.size(),
                         clip_stack_.size(), live_layer_bytes_,
                         live_layer_pixels_}))
    Invalidate();
}
void CoverageDisplayListPlan::SaveLayer(Rect bounds, bool elided, bool reads) {
  // Composition of a layer has sample-varying source content. Classify it in
  // the parent before descending, including a live backdrop read.
  RecordDraw({.physical_bounds = bounds, .reads_destination = reads});
  Save();
  if (overflow_)
    return;
  bounds = Bounded(bounds);
  const uint64_t pixels = elided ? 0 : Pixels(bounds);
  const uint64_t bytes = Multiply(pixels, bytes_per_pixel_);
  current_scope_ = scopes_.size();
  if (!scopes_.push_back({.physical_bounds = bounds,
                          .nominal_layer_bytes = bytes,
                          .reads_destination = reads,
                          .layer_is_elided = elided})) {
    Invalidate();
    return;
  }
  live_layer_bytes_ = Add(live_layer_bytes_, bytes);
  live_layer_pixels_ = Add(live_layer_pixels_, pixels);
  peak_live_layer_bytes_ = std::max(peak_live_layer_bytes_, live_layer_bytes_);
  peak_live_layer_pixels_ =
      std::max(peak_live_layer_pixels_, live_layer_pixels_);
  scopes_[0].peak_live_layer_bytes = peak_live_layer_bytes_;
  for (const auto& saved : saved_)
    scopes_[saved.scope].peak_live_layer_bytes =
        std::max(scopes_[saved.scope].peak_live_layer_bytes, live_layer_bytes_);
  scopes_[current_scope_].peak_live_layer_bytes = live_layer_bytes_;
}
void CoverageDisplayListPlan::Restore() {
  if (overflow_)
    return;
  if (saved_.empty())
    return;
  const auto state = saved_.back();
  saved_.pop_back();
  current_scope_ = state.scope;
  CloseClipSegments(state.clip_stack);
  active_clips_.resize(state.active_clips);
  live_layer_bytes_ = state.live_bytes;
  live_layer_pixels_ = state.live_pixels;
}
void CoverageDisplayListPlan::CloseClipSegments(size_t keep) {
  while (clip_stack_.size() > keep) {
    auto& clip = clips_[clip_stack_.back()];
    clip.end_draw = draw_records_.size();
    clip.closed = true;
    clip_stack_.pop_back();
  }
}
bool CoverageDisplayListPlan::FullGeometryOnFringe(
    const CoverageDrawFacts& facts,
    const CoverageClipDecision& clip) const {
  if (facts.kind != CoverageGeometryKind::kRect || facts.stroke ||
      !facts.uniform_source || !facts.uniform_geometry ||
      facts.reads_destination) {
    return false;
  }
  const Rect protected_bounds = Bounded(clip.physical_bounds.Expand(1));
  if (protected_bounds.IsEmpty() ||
      !facts.uniform_geometry->GetBounds().Contains(protected_bounds)) {
    return false;
  }
  for (const auto& corner : protected_bounds.GetPoints()) {
    for (const auto& line : facts.uniform_geometry->GetLineParameters().vec) {
      const Scalar distance = line.x * corner.x + line.y * corner.y + line.z;
      if (!std::isfinite(distance) || distance < 0) {
        return false;
      }
    }
  }
  return true;
}
void CoverageDisplayListPlan::Finalize() {
  if (overflow_ || finalized_)
    return;
  CloseClipSegments(0);
  for (auto& clip : clips_) {
    bool no_external = true;
    bool all_full =
        clip.end_draw > clip.begin_draw && !clip.contains_nested_clip;
    bool all_src_over = true;
    bool all_opaque = true;
    for (size_t i = 0; i < draw_records_.size(); i++) {
      const auto& record = draw_records_[i];
      if (i >= clip.begin_draw && i < clip.end_draw) {
        all_full &= FullGeometryOnFringe(record.facts, clip);
        all_src_over &= record.facts.blend == BlendMode::kSrcOver;
        all_opaque &= record.facts.source_is_opaque;
      } else if (!record.sample_values_are_equal &&
                 FringeTouches(clip, record.facts.physical_bounds.Expand(1))) {
        // A one-sample boundary may not resolve lanes subsequently needed by
        // overlapping geometry, even when it is outside this clip's subtree.
        no_external = false;
      }
    }
    clip.no_external_fringe_correlation = no_external && clip.closed;
    clip.all_draws_full_clip_geometry = all_full;
    clip.uniform_full_segment = all_full && all_src_over;
    clip.all_sources_opaque = all_full && all_opaque;
  }
  for (auto& image : images_) {
    if (!image.candidate_analytic_joint_mask)
      continue;
    bool no_external = true;
    for (size_t i = 0; i < draw_records_.size(); i++) {
      const auto& record = draw_records_[i];
      if (i != image.draw_record_index && !record.sample_values_are_equal &&
          !record.facts.physical_bounds.Expand(1)
               .IntersectionOrEmpty(image.physical_bounds.Expand(1))
               .IsEmpty()) {
        no_external = false;
      }
    }
    image.no_external_fringe_correlation = no_external;
    if (no_external) {
      image.fully_joint_mask = true;
      image.route = CoverageImageDecision::Route::kAnalyticJointMask1x;
    }
  }
  // A mixed native4 painter segment never becomes direct from one safe image.
  // Authorize a whole no-clip scope only if every real color operation has a
  // positive source/geometry proof, including both earlier and later overlap.
  if (scopes_.size() == 1 && clips_.empty()) {
    bool direct = true;
    for (size_t i = 0; i < draw_records_.size(); i++) {
      bool safe = draw_records_[i].facts.direct_1x_eligible &&
                  !draw_records_[i].facts.reads_destination;
      for (const auto& image : images_) {
        if (image.draw_record_index == i &&
            image.route == CoverageImageDecision::Route::kAnalyticJointMask1x)
          safe = true;
      }
      direct &= safe;
    }
    scopes_[0].can_render_direct_1x = direct;
  }
  finalized_ = true;
}
void CoverageDisplayListPlan::Reason(AvioCoverageReason reason, Rect bounds) {
  const auto index = static_cast<size_t>(reason);
  auto& entry = reasons_[index];
  entry.reason_id = static_cast<uint32_t>(reason);
  entry.draw_count = Add(entry.draw_count, 1);
  entry.pixel_area = Add(entry.pixel_area, Pixels(Bounded(bounds)));
  const auto add_scope = [&](size_t index) {
    auto& scoped = scopes_[index].reasons[static_cast<size_t>(reason)];
    scoped.reason_id = entry.reason_id;
    scoped.draw_count = Add(scoped.draw_count, 1);
    scoped.pixel_area = Add(scoped.pixel_area, Pixels(Bounded(bounds)));
  };
  add_scope(current_scope_);
  size_t previous = current_scope_;
  for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
    if (it->scope == previous)
      continue;
    add_scope(it->scope);
    previous = it->scope;
  }
}
void CoverageDisplayListPlan::RecordClip(Rect bounds,
                                         const Matrix& transform,
                                         ClipOperation operation,
                                         bool is_rect,
                                         bool aa) {
  if (overflow_)
    return;
  bounds = Bounded(bounds);
  CoverageClipDecision clip{bounds, transform, operation, is_rect, aa, {}};
  clip.segment_token =
      next_segment_token.fetch_add(1, std::memory_order_relaxed);
  if (!clip.segment_token) {
    Invalidate();
    return;
  }
  clip.begin_draw = draw_records_.size();
  clip.stack_depth = clip_stack_.size() + 1;
  if (clip.stack_depth <= clip.stack_tokens.size()) {
    for (size_t i = 0; i < clip_stack_.size(); i++) {
      clip.stack_tokens[i] = clips_[clip_stack_[i]].segment_token;
    }
    clip.stack_tokens[clip_stack_.size()] = clip.segment_token;
  }
  for (const auto parent : clip_stack_)
    clips_[parent].contains_nested_clip = true;
  if (!clips_.push_back(clip) || !clip_stack_.push_back(clips_.size() - 1)) {
    Invalidate();
    return;
  }
  scopes_[current_scope_].can_render_direct_1x = false;
  if (aa && !(is_rect && CanUseRectClipScissor4(bounds, operation))) {
    if (!active_clips_.push_back(clips_.size() - 1)) {
      Invalidate();
      return;
    }
    Reason(is_rect ? AvioCoverageReason::kClassFractionalClip
                   : AvioCoverageReason::kClassNonRectClip,
           bounds);
  }
}
void CoverageDisplayListPlan::RecordDraw(const CoverageDrawFacts& input) {
  if (overflow_)
    return;
  auto facts = input;
  facts.physical_bounds = Bounded(facts.physical_bounds);
  bool equal_samples = facts.sample_values_are_equal;
  for (const auto index : clip_stack_) {
    const auto& clip = clips_[index];
    equal_samples &= clip.is_rect && CanUseRectClipScissor4(
                                         clip.physical_bounds, clip.operation);
  }
  if (!draw_records_.push_back({facts, equal_samples})) {
    Invalidate();
    return;
  }
  auto& scope = scopes_[current_scope_];
  ++scope.draw_count;
  scope.can_render_direct_1x &=
      facts.direct_1x_eligible && clips_.empty() && !facts.reads_destination;
  scope.requires_legacy_sdf |= facts.uses_legacy_sdf;
  scope.reads_destination |= facts.reads_destination;
  bool needs_coverage = false;
  if (!facts.anti_alias)
    Reason(AvioCoverageReason::kClassNoAntialias, facts.physical_bounds);
  if (facts.blend != BlendMode::kSrcOver) {
    Reason(AvioCoverageReason::kClassRejectedBlend, facts.physical_bounds);
    needs_coverage = true;
  }
  if (facts.reads_destination)
    Reason(AvioCoverageReason::kClassDestinationRead, facts.physical_bounds);
  if (facts.anti_alias) {
    std::optional<AvioCoverageReason> reason;
    switch (facts.kind) {
      case CoverageGeometryKind::kPath:
        reason = facts.stroke ? AvioCoverageReason::kClassPathStroke
                              : AvioCoverageReason::kClassPathFill;
        break;
      case CoverageGeometryKind::kArc:
        reason = AvioCoverageReason::kClassArc;
        break;
      case CoverageGeometryKind::kBorder:
        reason = AvioCoverageReason::kClassBorder;
        break;
      case CoverageGeometryKind::kEllipticalRoundRect:
        reason = AvioCoverageReason::kClassEllipticalRoundRect;
        break;
      case CoverageGeometryKind::kImage:
        if (facts.image_has_edge)
          reason = AvioCoverageReason::kClassImageEdge;
        break;
      default:
        break;
    }
    if (reason) {
      Reason(*reason, facts.physical_bounds);
      needs_coverage = true;
    }
    if (!facts.analytic_source_supported) {
      Reason(AvioCoverageReason::kClassRejectedSource, facts.physical_bounds);
      needs_coverage = true;
    }
  }
  for (const auto index : active_clips_) {
    auto& clip = clips_[index];
    const bool touches = FringeTouches(clip, facts.physical_bounds);
    // Uniform paint is insufficient: its own geometry must cover the entire
    // fringe. Unknown bounds, images, text and paths never supply this proof.
    const bool full = FullGeometryOnFringe(facts, clip);
    clip.draws.RecordDraw(touches, facts.blend, full, facts.reads_destination);
    needs_coverage |= touches;
  }
  if (needs_coverage) {
    ++scope.coverage_draw_count;
    prior_fringe_bounds_ = prior_fringe_bounds_.Union(facts.physical_bounds);
  }
  // Root and enclosing saveLayers summarize their complete subtrees. Plain
  // saves can repeat a scope ID and must not double-count it.
  size_t previous = current_scope_;
  for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
    if (it->scope == previous)
      continue;
    auto& parent = scopes_[it->scope];
    ++parent.draw_count;
    parent.coverage_draw_count += needs_coverage ? 1 : 0;
    parent.reads_destination |= facts.reads_destination;
    parent.requires_legacy_sdf |= facts.uses_legacy_sdf;
    previous = it->scope;
  }
}
void CoverageDisplayListPlan::RecordScratchDemand(Rect bounds, uint64_t count) {
  if (overflow_)
    return;
  const uint64_t pixels =
      Add(live_layer_pixels_, Multiply(Pixels(Bounded(bounds)), count));
  const uint64_t bytes = Multiply(pixels, bytes_per_pixel_);
  peak_live_layer_bytes_ = std::max(peak_live_layer_bytes_, bytes);
  peak_live_layer_pixels_ = std::max(peak_live_layer_pixels_, pixels);
  scopes_[current_scope_].peak_live_layer_bytes =
      std::max(scopes_[current_scope_].peak_live_layer_bytes, bytes);
  for (const auto& state : saved_)
    scopes_[state.scope].peak_live_layer_bytes =
        std::max(scopes_[state.scope].peak_live_layer_bytes, bytes);
}
void CoverageDisplayListPlan::RecordImage(Rect local,
                                          const Matrix& matrix,
                                          const CoverageDrawFacts& facts) {
  if (overflow_)
    return;
  const bool overlap =
      !prior_fringe_bounds_.IntersectionOrEmpty(facts.physical_bounds)
           .IsEmpty();
  const bool unmasked = facts.direct_1x_eligible && !facts.anti_alias &&
                        clips_.empty() && !overlap &&
                        facts.blend == BlendMode::kSrcOver &&
                        !facts.reads_destination;
  CoverageImageDecision image{
      local,
      matrix,
      facts.blend,
      facts.anti_alias,
      overlap,
      unmasked,
      unmasked ? CoverageImageDecision::Route::kDirectWithoutCoverage
               : CoverageImageDecision::Route::kNative4};
  image.draw_record_index = draw_records_.size();
  image.physical_bounds = facts.physical_bounds;
  image.source_is_opaque = facts.source_is_opaque;
  image.source_is_encoded = facts.image_source_is_encoded;
  image.candidate_analytic_joint_mask =
      facts.analytic_image_1x_eligible && facts.source_is_opaque &&
      facts.image_source_is_encoded && facts.anti_alias &&
      facts.blend == BlendMode::kSrcOver && !facts.reads_destination &&
      clips_.empty();
  if (!images_.push_back(image)) {
    Invalidate();
    return;
  }
  RecordDraw(facts);
}
std::optional<CoverageImageDecision> CoverageDisplayListPlan::FindImageDecision(
    Rect bounds,
    const Matrix& matrix,
    bool aa,
    BlendMode blend) const {
  if (overflow_)
    return std::nullopt;
  std::optional<CoverageImageDecision> result;
  for (const auto& image : images_) {
    if (image.local_bounds == bounds && image.transform == matrix &&
        image.anti_alias == aa && image.blend == blend) {
      if (result && result->route != image.route)
        return std::nullopt;
      result = image;
    }
  }
  return result;
}
std::optional<CoverageClipDecision> CoverageDisplayListPlan::FindClipDecision(
    Rect bounds,
    const Matrix& matrix,
    ClipOperation operation,
    bool rect,
    bool aa,
    std::span<const uint64_t> parent_stack) const {
  if (overflow_)
    return std::nullopt;
  std::optional<CoverageClipDecision> result;
  for (const auto& clip : clips_) {
    if (clip.physical_bounds == Bounded(bounds) && clip.transform == matrix &&
        clip.operation == operation && clip.is_rect == rect &&
        clip.anti_alias == aa) {
      if (parent_stack.size() + 1 != clip.stack_depth ||
          clip.stack_depth > clip.stack_tokens.size()) {
        continue;
      }
      bool same_parents = true;
      for (size_t i = 0; i < parent_stack.size(); i++)
        same_parents &= parent_stack[i] == clip.stack_tokens[i];
      if (!same_parents)
        continue;
      // Repeated declarations cannot be paired by replay ordinal after damage
      // culling. An ambiguous identity never authorizes a segment boundary.
      if (result && result->segment_token != clip.segment_token)
        return std::nullopt;
      result = clip;
    }
  }
  return result;
}
void CoverageDisplayListPlan::Report(Context& context) const {
  for (const auto& reason : reasons_) {
    if (!reason.draw_count)
      continue;
    context.RecordAvioCoverageClassification(
        static_cast<AvioCoverageReason>(reason.reason_id), reason.draw_count,
        reason.pixel_area);
  }
  if (!overflow_)
    context.RecordAvioCoverageClassification(
        AvioCoverageReason::kClassLayerPeakDemand, 1, peak_live_layer_pixels_);
  if (overflow_)
    context.RecordAvioCoverageClassification(
        AvioCoverageReason::kClassCapacityOverflow, 1, 0);
  if (!overflow_)
    context.RecordAvioCoverageLayerDemand(peak_live_layer_bytes_,
                                          scopes_.size());
}

}  // namespace impeller
