// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_SAMPLE4_CLIP_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_SAMPLE4_CLIP_H_

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <span>

#include "impeller/entity/contents/continuous_clip.h"
#include "impeller/entity/contents/coverage_mask_contents.h"
#include "impeller/entity/geometry/coverage_geometry.h"
#include "impeller/entity/geometry/geometry_result.h"

namespace impeller {

// Coordinates and native sample IDs remain on the original logical pass grid.
// The atlas representation is binary unresolved R8, sampled per native lane;
// it is never a resolved alpha which could lose clip/geometry correlation.
struct AvioSample4ClipNode {
  std::optional<CoverageConvexQuad4> quad;
  std::optional<CoverageMaskTile> mask;
  std::optional<GeometryResult> geometry;
  Rect physical_bounds;
  uint32_t clip_depth = 0;
  ClipOperation operation = ClipOperation::kIntersect;
  uint64_t declaration_token = 0;

  // Certified by the native geometry owner, never by resolved mask alpha.
  // Outside outer_pixels every native lane is zero. Each full box contains
  // only pixels whose four lanes are inside this node's actual geometry.
  IRect outer_pixels;
  std::array<IRect, 16> full_coverage_pixels;
  uint32_t full_coverage_count = 0;
  bool coverage_proof_valid = false;

  bool SetCoverageProof(Rect bounds, std::span<const IRect> full_boxes);
  bool HasFullCoverage(IRect pixels) const;

  Rect GetBounds() const;
};

// Borrow the original native geometry owner's conservative containment
// certificate without retaining an ephemeral Geometry or allocating a closure.
struct AvioSample4NativeGeometryProof {
  const void* geometry = nullptr;
  bool (*covers_area)(const void*, const Matrix&, IRect) = nullptr;
};

void PopulateSample4CoverageProof(
    AvioSample4ClipNode& node,
    const Matrix& transform,
    const std::optional<AvioContinuousClip>& shape,
    AvioSample4NativeGeometryProof geometry);

// Disjoint original-grid regions. Native lane masks are required only for
// fringe_pixels; interior_pixels have an exact all-four-lanes certificate.
// Both lists have standing bounds; fragmentation overflow conservatively
// returns one fringe rectangle, consumed through bounded tile flattening.
struct AvioSample4ClipFringePlan {
  static constexpr size_t kCapacity = 128;
  std::array<IRect, kCapacity> interior_pixels;
  std::array<IRect, kCapacity> fringe_pixels;
  uint32_t interior_count = 0;
  uint32_t fringe_count = 0;
};

// An immutable logical stack, retained by every packet which reads it. The
// classifier's token identifies a complete declaration segment, not a draw
// ordinal. Admission flags do not replace the real clip-depth replay: an
// malformed/overflowed stack must keep that replay and decline optimized
// routes.
struct AvioSample4ClipDescriptor {
  static constexpr size_t kCapacity = 4;
  static constexpr size_t kMaxBundles = 128;
  static constexpr size_t kMaxDepth = kCapacity * kMaxBundles;

  std::shared_ptr<const AvioSample4ClipDescriptor> parent;
  ISize logical_pass_size;
  std::array<AvioSample4ClipNode, kCapacity> nodes;
  uint32_t count = 0;
  uint64_t segment_token = 0;
  ClipCoverageStrategy strategy = ClipCoverageStrategy::kFringeIsland;
  bool no_external_fringe_correlation = false;
  bool all_draws_full_clip_geometry = false;
  bool all_sources_opaque = false;

  bool AppendQuad(const CoverageConvexQuad4& quad, ClipOperation operation);
  bool AppendMask(const CoverageMaskTile& mask, ClipOperation operation);
  bool AppendGeometry(const GeometryResult& geometry,
                      Rect bounds,
                      ClipOperation operation);

  // Complete writable pixel footprint for source-owned full-geometry proofs.
  // Difference clips cannot reduce this conservative bounding rectangle.
  std::optional<Rect> GetSourceProofBounds() const;
  std::optional<IRect> GetSourceProofRasterBounds() const;

  template <class Visitor>
  bool ForEachNode(Visitor&& visitor) const {
    std::array<const AvioSample4ClipDescriptor*, kMaxBundles> bundles;
    size_t bundle_count = 0;
    for (auto current = this; current; current = current->parent.get()) {
      if (bundle_count == bundles.size() || current->count > kCapacity) {
        return false;
      }
      bundles[bundle_count++] = current;
    }
    while (bundle_count) {
      const auto& bundle = *bundles[--bundle_count];
      for (size_t i = 0; i < bundle.count; i++) {
        if (!visitor(bundle.nodes[i])) {
          return false;
        }
      }
    }
    return true;
  }

  size_t GetDepth() const {
    size_t depth = 0;
    return ForEachNode([&](const auto&) {
      depth++;
      return true;
    })
               ? depth
               : 0;
  }

  // CPU counterpart of the shader's ordered AND / AND-NOT. Native atlas reads
  // are supplied by the caller with exact sample IDs; no filtering or alpha
  // reconstruction is permitted. Nullopt reports an unrepresented/invalid
  // node, rather than treating an unknown difference mask as empty.
  std::optional<ClipSampleMask4> Combine(
      std::span<const ClipSampleMask4> shape_masks,
      ClipSampleMask4 geometry = kClipSampleMask4Full) const;

  std::optional<AvioSample4ClipFringePlan> GetFringePlan(IRect writable) const;

  bool HasSingleQuad() const {
    return !parent && count == 1 && nodes[0].quad.has_value();
  }
};

using AvioSample4ClipRecipe = AvioSample4ClipDescriptor;

// Standing CPU storage allocated before raster. Strong readers pin a slot
// across saves, clip restores, packet replay and deferred submission. Admission
// cannot allocate or wait; contention/capacity refusal selects real native4.
class AvioSample4ClipPool {
 public:
  // 128 live K4 bundles represent the owner's existing 512-node limit.
  // One additional standing slot permits an immutable append/copy while its
  // predecessor is still owned by Canvas or a packet.
  static constexpr size_t kCapacity =
      AvioSample4ClipDescriptor::kMaxBundles + 1;

  AvioSample4ClipPool();

  std::shared_ptr<AvioSample4ClipDescriptor> Acquire(
      const std::shared_ptr<const AvioSample4ClipDescriptor>& previous);

  // Return idle mask leases before the atlas admits a new writer. Live saved
  // states and packets keep their exact leases; a frame end alone is no proof.
  bool ReclaimUnused();

 private:
  std::array<std::shared_ptr<AvioSample4ClipDescriptor>, kCapacity> slots_;
  std::atomic_flag acquiring_ = ATOMIC_FLAG_INIT;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_SAMPLE4_CLIP_H_
