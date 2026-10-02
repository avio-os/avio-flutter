// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_DISPLAY_LIST_COVERAGE_CLASSIFIER_H_
#define FLUTTER_IMPELLER_DISPLAY_LIST_COVERAGE_CLASSIFIER_H_

#include <array>
#include <iterator>
#include <memory>
#include <span>

#include "flutter/display_list/display_list.h"
#include "impeller/entity/contents/clip_coverage.h"
#include "impeller/entity/geometry/coverage_geometry.h"
#include "impeller/geometry/matrix.h"
#include "impeller/renderer/render_resource_report.h"

namespace impeller {

class Context;

template <class T, size_t Capacity>
class CoverageFixedVector {
 public:
  bool push_back(const T& value) {
    if (count_ == Capacity)
      return false;
    entries_[count_++] = value;
    return true;
  }
  void clear() { count_ = 0; }
  void pop_back() { --count_; }
  void resize(size_t count) { count_ = count; }
  bool empty() const { return count_ == 0; }
  size_t size() const { return count_; }
  T& operator[](size_t i) { return entries_[i]; }
  const T& operator[](size_t i) const { return entries_[i]; }
  T& front() { return entries_[0]; }
  const T& front() const { return entries_[0]; }
  T& back() { return entries_[count_ - 1]; }
  auto begin() { return entries_.begin(); }
  auto end() { return entries_.begin() + count_; }
  auto begin() const { return entries_.begin(); }
  auto end() const { return entries_.begin() + count_; }
  auto rbegin() { return std::make_reverse_iterator(end()); }
  auto rend() { return std::make_reverse_iterator(begin()); }

 private:
  std::array<T, Capacity> entries_ = {};
  size_t count_ = 0;
};

enum class CoverageGeometryKind {
  kRect,
  kPath,
  kArc,
  kBorder,
  kRoundRect,
  kEllipticalRoundRect,
  kImage,
  kText,
  kOther,
};

// The pre-pass describes logical work, not driver allocations. Uncertain
// bounds and source samples always select the existing native4 replay.
struct CoverageDrawFacts {
  CoverageGeometryKind kind = CoverageGeometryKind::kOther;
  Rect physical_bounds;
  bool anti_alias = true;
  bool stroke = false;
  bool uniform_source = false;
  bool source_is_opaque = false;
  // An AABB is insufficient: a rotated rect can bound, yet cut, the fringe.
  std::optional<CoverageConvexQuad4> uniform_geometry;
  bool analytic_source_supported = true;
  bool reads_destination = false;
  bool direct_1x_eligible = false;
  bool uses_legacy_sdf = false;
  bool sample_values_are_equal = false;
  bool analytic_image_1x_eligible = false;
  // Opacity alone does not prove the native attachment quantization cut.
  bool image_source_is_encoded = false;
  bool image_has_edge = true;
  BlendMode blend = BlendMode::kSrcOver;
};

struct CoverageScopeSummary {
  Rect physical_bounds;
  uint64_t draw_count = 0;
  uint64_t coverage_draw_count = 0;
  uint64_t nominal_layer_bytes = 0;
  uint64_t peak_live_layer_bytes = 0;
  std::array<AvioCoverageReasonUsage, 32> reasons = {};
  bool reads_destination = false;
  bool layer_is_elided = false;
  // Positive proof, never inferred from a zero diagnostic counter.
  bool can_render_direct_1x = true;
  // A direct pass must retain the exact legacy SDF shader route when this
  // proof supplied any of its geometry coverage.
  bool requires_legacy_sdf = false;
};

// Eligible legacy SDF shaders apply the same scalar coverage at every sample.
// Geometry eligibility is supplied only by their real dispatcher branches;
// source/paint/transform eligibility is shared here with focused CPU tests.
bool CanRenderLegacySdfAt1x(const CoverageDrawFacts& facts,
                            const Matrix& transform,
                            bool owner_use_sdfs);

struct CoverageClipDecision {
  Rect physical_bounds;
  Matrix transform;
  ClipOperation operation;
  bool is_rect;
  bool anti_alias;
  ClipCoverageDrawClassifier draws;
  uint64_t segment_token = 0;
  // Exact retained ancestry matches the pre-pass's fixed active-clip bound.
  // Deep runtime descriptors flatten into native sample masks, not hashes.
  std::array<uint64_t, 512> stack_tokens = {};
  size_t stack_depth = 0;
  bool no_external_fringe_correlation = false;
  bool uniform_full_segment = false;
  bool all_draws_full_clip_geometry = false;
  bool all_sources_opaque = false;
  ClipCoverageStrategy GetStrategy() const { return draws.GetStrategy(); }
  bool HasCertifiedSegment() const {
    return segment_token && stack_depth > 0 &&
           stack_depth <= stack_tokens.size() &&
           no_external_fringe_correlation && all_draws_full_clip_geometry;
  }
  // Mutable only during the pre-pass; frozen before any runtime caller.
  size_t begin_draw = 0;
  size_t end_draw = 0;
  bool closed = false;
  bool contains_nested_clip = false;
};

struct CoverageImageDecision {
  enum class Route { kDirectWithoutCoverage, kAnalyticJointMask1x, kNative4 };
  Rect local_bounds;
  Matrix transform;
  BlendMode blend;
  bool anti_alias;
  bool prior_fringe_overlap;
  // No scalar-alpha clip or unproven source sampling authorizes direct1x.
  bool fully_joint_mask = false;
  Route route = Route::kNative4;
  bool no_external_fringe_correlation = false;
  // Frozen pre-pass custody, not a replay ordinal consumed by the renderer.
  size_t draw_record_index = 0;
  Rect physical_bounds;
  bool candidate_analytic_joint_mask = false;
  bool source_is_opaque = false;
  bool source_is_encoded = false;
};

class CoverageDisplayListPlan {
 public:
  CoverageDisplayListPlan(Rect physical_bounds, size_t color_bytes_per_pixel);
  void Reset(Rect physical_bounds, size_t color_bytes_per_pixel);
  void Invalidate();
  void Finalize();
  bool HasOverflow() const { return overflow_; }

  void Save();
  void SaveLayer(Rect bounds, bool elided, bool reads_destination);
  void Restore();
  void RecordClip(Rect bounds,
                  const Matrix& transform,
                  ClipOperation operation,
                  bool is_rect,
                  bool anti_alias);
  void RecordDraw(const CoverageDrawFacts& facts);
  // Conservative logical colour-image demand for a concrete filter graph.
  // Images are counted from that graph, never treated as real allocation bytes.
  void RecordScratchDemand(Rect bounds, uint64_t image_count);
  void RecordImage(Rect local_bounds,
                   const Matrix& transform,
                   const CoverageDrawFacts& facts);

  const CoverageScopeSummary& GetRoot() const { return scopes_.front(); }
  const auto& GetScopes() const { return scopes_; }
  const auto& GetClips() const { return clips_; }
  uint64_t GetPeakLayerBytes() const { return peak_live_layer_bytes_; }
  uint64_t GetPeakLayerPixels() const { return peak_live_layer_pixels_; }

  std::optional<CoverageImageDecision> FindImageDecision(
      Rect local_bounds,
      const Matrix& transform,
      bool anti_alias,
      BlendMode blend) const;
  std::optional<CoverageClipDecision> FindClipDecision(
      Rect physical_bounds,
      const Matrix& transform,
      ClipOperation operation,
      bool is_rect,
      bool anti_alias,
      std::span<const uint64_t> parent_stack = {}) const;

  // Separate reason IDs from execution-route counters; bbox pixels only.
  void Report(Context& context) const;
  const std::array<AvioCoverageReasonUsage, 32>& GetReasons() const {
    return reasons_;
  }

 private:
  struct SavedState {
    size_t scope;
    size_t active_clips;
    size_t clip_stack;
    uint64_t live_bytes;
    uint64_t live_pixels;
  };
  void Reason(AvioCoverageReason reason, Rect bounds);
  void CloseClipSegments(size_t keep);
  bool FullGeometryOnFringe(const CoverageDrawFacts& facts,
                            const CoverageClipDecision& clip) const;
  Rect Bounded(Rect bounds) const;
  CoverageFixedVector<CoverageScopeSummary, 256> scopes_;
  CoverageFixedVector<CoverageClipDecision, 512> clips_;
  CoverageFixedVector<size_t, 512> active_clips_;
  CoverageFixedVector<size_t, 512> clip_stack_;
  struct DrawRecord {
    CoverageDrawFacts facts;
    bool sample_values_are_equal;
  };
  CoverageFixedVector<DrawRecord, 2048> draw_records_;
  CoverageFixedVector<CoverageImageDecision, 1024> images_;
  CoverageFixedVector<SavedState, 512> saved_;
  std::array<AvioCoverageReasonUsage, 32> reasons_ = {};
  Rect physical_bounds_;
  Rect prior_fringe_bounds_;
  size_t bytes_per_pixel_;
  size_t current_scope_ = 0;
  uint64_t live_layer_bytes_ = 0;
  uint64_t live_layer_pixels_ = 0;
  uint64_t peak_live_layer_bytes_ = 0;
  uint64_t peak_live_layer_pixels_ = 0;
  bool overflow_ = false;
  bool finalized_ = false;
};

// Runs once alongside the existing text/backdrop pre-pass. The result is
// keyed by immutable operation facts, never by ordinal across culled passes.
std::shared_ptr<const CoverageDisplayListPlan> ClassifyCoverageDisplayList(
    const sk_sp<flutter::DisplayList>& display_list,
    std::shared_ptr<CoverageDisplayListPlan> cold_storage,
    Rect physical_bounds,
    size_t color_bytes_per_pixel,
    bool owner_use_sdfs = false,
    bool owner_sample4_image1x = false);

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_DISPLAY_LIST_COVERAGE_CLASSIFIER_H_
