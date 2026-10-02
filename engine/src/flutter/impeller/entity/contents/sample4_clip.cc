// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/sample4_clip.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "impeller/geometry/round_superellipse_param.h"

namespace impeller {

std::optional<Rect> AvioSample4ClipDescriptor::GetSourceProofBounds() const {
  if (!segment_token || logical_pass_size.IsEmpty()) {
    return std::nullopt;
  }
  std::optional<Rect> bounds = Rect::MakeSize(logical_pass_size);
  if (!ForEachNode([&](const AvioSample4ClipNode& node) {
        const auto node_bounds = node.GetBounds();
        if (!node_bounds.IsFinite() || node_bounds.IsEmpty()) {
          return false;
        }
        if (node.operation == ClipOperation::kIntersect && bounds) {
          bounds = bounds->Intersection(node_bounds);
        }
        return true;
      }) ||
      !bounds) {
    return std::nullopt;
  }
  return bounds;
}

std::optional<IRect> AvioSample4ClipDescriptor::GetSourceProofRasterBounds()
    const {
  const auto bounds = GetSourceProofBounds();
  return bounds ? IRect::RoundOut(bounds->Expand(1.f))
                      .Intersection(IRect::MakeSize(logical_pass_size))
                : std::nullopt;
}

// Retain native geometry certificates at clip declaration time. A fixed
// partition finds straight-edge interiors and leaves uncertain curved/path
// areas for exact sample-lane replay; it never samples a resolved mask alpha.
void PopulateSample4CoverageProof(
    AvioSample4ClipNode& node,
    const Matrix& transform,
    const std::optional<AvioContinuousClip>& shape,
    AvioSample4NativeGeometryProof geometry) {
  const auto bounds = node.GetBounds();
  if (!bounds.IsFinite() || bounds.IsEmpty() || !transform.IsAffine() ||
      !transform.IsFinite() || !transform.IsInvertible()) {
    node.SetCoverageProof(bounds, {});
    return;
  }
  const auto outer = IRect::RoundOut(bounds);
  std::array<int64_t, 16> xs, ys;
  size_t nx = 2, ny = 2;
  xs[0] = outer.GetLeft();
  xs[1] = outer.GetRight();
  ys[0] = outer.GetTop();
  ys[1] = outer.GetBottom();
  const auto add = [](auto& edges, size_t& count, Scalar coordinate, int64_t lo,
                      int64_t hi) {
    for (const auto value : {std::floor(coordinate), std::ceil(coordinate)}) {
      if (value > lo && value < hi && count < edges.size() &&
          std::find(edges.begin(), edges.begin() + count, value) ==
              edges.begin() + count) {
        edges[count++] = static_cast<int64_t>(value);
      }
    }
  };
  for (const Scalar fraction : {.25f, .375f, .5f, .625f, .75f}) {
    add(xs, nx, bounds.GetLeft() + bounds.GetWidth() * fraction,
        outer.GetLeft(), outer.GetRight());
    add(ys, ny, bounds.GetTop() + bounds.GetHeight() * fraction, outer.GetTop(),
        outer.GetBottom());
  }
  Rect local;
  if (shape) {
    const auto& b = shape->primitive.vectors[3];
    local = Rect::MakeLTRB(b[0] - b[2], b[1] - b[3], b[0] + b[2], b[1] + b[3]);
    const auto& r = shape->sample4_radii;
    const Scalar gap =
        shape->shape_class == AvioContinuousClass::kSuperellipseClip
            ? RoundSuperellipseParam::kGapFactor
            : 1.f;
    const std::array<Point, 4> native_boundaries = {
        Point(local.GetLeft() +
                  std::max(r.top_left.width, r.bottom_left.width) * gap,
              local.GetTop() +
                  std::max(r.top_left.height, r.top_right.height) * gap),
        Point(local.GetRight() -
                  std::max(r.top_right.width, r.bottom_right.width) * gap,
              local.GetBottom() -
                  std::max(r.bottom_left.height, r.bottom_right.height) * gap),
        local.GetLeftTop(), local.GetRightBottom()};
    for (const auto& point : native_boundaries) {
      const auto physical = transform * point;
      add(xs, nx, physical.x, outer.GetLeft(), outer.GetRight());
      add(ys, ny, physical.y, outer.GetTop(), outer.GetBottom());
    }
  }
  std::sort(xs.begin(), xs.begin() + nx);
  std::sort(ys.begin(), ys.begin() + ny);
  std::array<IRect, 16> full;
  size_t count = 0;
  const auto certify = [&](IRect cell) {
    if (geometry.covers_area &&
        geometry.covers_area(geometry.geometry, transform, cell)) {
      return true;
    }
    if (!shape || shape->shape_class != AvioContinuousClass::kOvalClip ||
        local.IsEmpty()) {
      return false;
    }
    // GenerateFilledEllipse (including its circle specialization) always
    // includes the four cardinal vertices. Their inscribed diamond is inside
    // every native tessellation, without relying on an ideal ellipse SDF.
    const auto inverse = transform.Invert();
    const auto center = local.GetCenter();
    for (const auto& point : Rect::MakeLTRB(cell.GetLeft(), cell.GetTop(),
                                            cell.GetRight(), cell.GetBottom())
                                 .GetPoints()) {
      const auto p = inverse * point - center;
      if (std::abs(p.x) / (local.GetWidth() * .5f) +
              std::abs(p.y) / (local.GetHeight() * .5f) >=
          1.f) {
        return false;
      }
    }
    return true;
  };
  for (size_t y = 0; y + 1 < ny; y++) {
    size_t start = 0;
    bool in_run = false;
    for (size_t x = 0; x + 1 < nx; x++) {
      const auto cell = IRect::MakeLTRB(xs[x], ys[y], xs[x + 1], ys[y + 1]);
      const bool covered = certify(cell);
      if (covered && !in_run) {
        start = x;
        in_run = true;
      }
      if (in_run && (!covered || x + 2 == nx)) {
        const auto run = IRect::MakeLTRB(xs[start], ys[y],
                                         xs[covered ? x + 1 : x], ys[y + 1]);
        bool merged = false;
        for (size_t i = 0; i < count; i++) {
          if (full[i].GetLeft() == run.GetLeft() &&
              full[i].GetRight() == run.GetRight() &&
              full[i].GetBottom() == run.GetTop()) {
            full[i] = IRect::MakeLTRB(run.GetLeft(), full[i].GetTop(),
                                      run.GetRight(), run.GetBottom());
            merged = true;
            break;
          }
        }
        if (!merged) {
          if (count == full.size()) {
            node.SetCoverageProof(bounds, {});
            return;
          }
          full[count++] = run;
        }
        in_run = false;
      }
    }
  }
  node.SetCoverageProof(bounds, std::span(full).first(count));
}

Rect AvioSample4ClipNode::GetBounds() const {
  return quad ? quad->GetBounds() : mask ? mask->target_rect : physical_bounds;
}

bool AvioSample4ClipNode::SetCoverageProof(Rect bounds,
                                           std::span<const IRect> full_boxes) {
  coverage_proof_valid = false;
  full_coverage_count = 0;
  if (!bounds.IsFinite() || full_boxes.size() > full_coverage_pixels.size()) {
    return false;
  }
  // RoundOut saturates; reject before narrowing physical coordinates.
  const double limit = std::numeric_limits<int32_t>::max() - 1.0;
  for (const auto& point : bounds.GetPoints()) {
    if (std::abs(static_cast<double>(point.x)) > limit ||
        std::abs(static_cast<double>(point.y)) > limit) {
      return false;
    }
  }
  coverage_proof_valid = false;
  outer_pixels = IRect::RoundOut(bounds);
  full_coverage_count = 0;
  for (const auto& box : full_boxes) {
    if (box.IsEmpty()) {
      continue;
    }
    if (!outer_pixels.Contains(box)) {
      full_coverage_count = 0;
      return false;
    }
    full_coverage_pixels[full_coverage_count++] = box;
  }
  coverage_proof_valid = true;
  return true;
}

bool AvioSample4ClipNode::HasFullCoverage(IRect pixels) const {
  if (full_coverage_count > full_coverage_pixels.size() || pixels.IsEmpty()) {
    return false;
  }
  for (size_t i = 0; i < full_coverage_count; i++) {
    if (full_coverage_pixels[i].Contains(pixels)) {
      return true;
    }
  }
  return false;
}

bool AvioSample4ClipDescriptor::AppendQuad(const CoverageConvexQuad4& quad,
                                           ClipOperation operation) {
  if (count >= nodes.size()) {
    return false;
  }
  auto& node = nodes[count++];
  node = {.quad = quad, .operation = operation};
  if (!node.SetCoverageProof(quad.GetBounds(), {})) {
    count--;
    return false;
  }
  bool aligned = true;
  for (const auto& line : quad.GetLineParameters().vec) {
    aligned &= line.x == 0 || line.y == 0;
  }
  if (aligned) {
    if (auto fringe = ComputeRectClipFringe4(quad.GetBounds())) {
      node.full_coverage_pixels[0] = fringe->full_coverage_pixels;
      node.full_coverage_count = fringe->full_coverage_pixels.IsEmpty() ? 0 : 1;
    }
  }
  return true;
}

bool AvioSample4ClipDescriptor::AppendMask(const CoverageMaskTile& mask,
                                           ClipOperation operation) {
  if (count >= nodes.size() || !mask.lease || !mask.lease->IsValid() ||
      mask.lease->GetKind() != AvioCoverageRegion::Kind::kMask ||
      !mask.target_rect.IsFinite() || mask.target_rect.IsEmpty() ||
      mask.target_rect.GetSize() != Size(mask.lease->GetRequestedSize())) {
    return false;
  }
  for (const auto& point : mask.target_rect.GetPoints()) {
    if (std::floor(point.x) != point.x || std::floor(point.y) != point.y) {
      return false;
    }
  }
  auto& node = nodes[count++];
  node = {.mask = mask, .operation = operation};
  if (!node.SetCoverageProof(mask.target_rect, {})) {
    count--;
    return false;
  }
  return true;
}

bool AvioSample4ClipDescriptor::AppendGeometry(const GeometryResult& geometry,
                                               Rect bounds,
                                               ClipOperation operation) {
  if (count >= nodes.size() || !geometry.vertex_buffer || !bounds.IsFinite()) {
    return false;
  }
  auto& node = nodes[count++];
  node = {
      .geometry = geometry, .physical_bounds = bounds, .operation = operation};
  if (!node.SetCoverageProof(bounds, {})) {
    count--;
    return false;
  }
  return true;
}

std::optional<ClipSampleMask4> AvioSample4ClipDescriptor::Combine(
    std::span<const ClipSampleMask4> shape_masks,
    ClipSampleMask4 geometry) const {
  if (count > nodes.size() || shape_masks.size() != GetDepth() ||
      (geometry & ~kClipSampleMask4Full) != 0) {
    return std::nullopt;
  }
  auto combined = geometry;
  size_t i = 0;
  if (!ForEachNode([&](const auto& node) {
        const size_t representations = node.quad.has_value() +
                                       node.mask.has_value() +
                                       node.geometry.has_value();
        if (representations != 1 || (node.mask && !node.mask->lease) ||
            (shape_masks[i] & ~kClipSampleMask4Full) != 0) {
          return false;
        }
        combined =
            CombineClipSampleMask4(combined, shape_masks[i++], node.operation);
        return true;
      })) {
    return std::nullopt;
  }
  return combined;
}

std::optional<AvioSample4ClipFringePlan>
AvioSample4ClipDescriptor::GetFringePlan(IRect writable) const {
  AvioSample4ClipFringePlan result;
  if (GetDepth() == 0) {
    return std::nullopt;
  }
  if (writable.IsEmpty()) {
    return result;
  }
  std::array<int64_t, 32> xs, ys;
  size_t nx = 2, ny = 2;
  xs[0] = writable.GetLeft();
  xs[1] = writable.GetRight();
  ys[0] = writable.GetTop();
  ys[1] = writable.GetBottom();
  bool fragmented = false;
  const auto add = [&](auto& edges, size_t& count, int64_t value,
                       int64_t minimum, int64_t maximum) {
    if (value <= minimum || value >= maximum ||
        std::find(edges.begin(), edges.begin() + count, value) !=
            edges.begin() + count) {
      return;
    }
    if (count == edges.size()) {
      fragmented = true;
      return;
    }
    edges[count++] = value;
  };
  const auto add_box = [&](IRect box) {
    add(xs, nx, box.GetLeft(), writable.GetLeft(), writable.GetRight());
    add(xs, nx, box.GetRight(), writable.GetLeft(), writable.GetRight());
    add(ys, ny, box.GetTop(), writable.GetTop(), writable.GetBottom());
    add(ys, ny, box.GetBottom(), writable.GetTop(), writable.GetBottom());
  };
  if (!ForEachNode([&](const auto& node) {
        if (!node.coverage_proof_valid ||
            node.full_coverage_count > node.full_coverage_pixels.size()) {
          return false;
        }
        add_box(node.outer_pixels);
        for (size_t i = 0; i < node.full_coverage_count; i++) {
          add_box(node.full_coverage_pixels[i]);
        }
        return true;
      })) {
    return std::nullopt;
  }
  const auto conservative = [&]() {
    result = {};
    result.fringe_pixels[0] = writable;
    result.fringe_count = 1;
    return result;
  };
  if (fragmented) {
    return conservative();
  }
  std::sort(xs.begin(), xs.begin() + nx);
  std::sort(ys.begin(), ys.begin() + ny);
  // Coalesce identical horizontal runs across neighboring rows. Every
  // published rectangle remains disjoint, so Clear/Src is never applied twice.
  const auto append = [](auto& boxes, uint32_t& count, IRect box) {
    for (size_t i = count; i > 0; i--) {
      const auto previous = boxes[i - 1];
      if (previous.GetLeft() == box.GetLeft() &&
          previous.GetRight() == box.GetRight() &&
          previous.GetBottom() == box.GetTop()) {
        boxes[i - 1] = IRect::MakeLTRB(previous.GetLeft(), previous.GetTop(),
                                       box.GetRight(), box.GetBottom());
        return true;
      }
    }
    if (count == boxes.size()) {
      return false;
    }
    boxes[count++] = box;
    return true;
  };
  enum class Membership { kOutside, kFull, kFringe };
  for (size_t y = 0; y + 1 < ny; y++) {
    size_t start = 0;
    Membership previous = Membership::kOutside;
    const auto publish = [&](size_t end) {
      if (previous == Membership::kOutside || end == start) {
        return true;
      }
      const auto run = IRect::MakeLTRB(xs[start], ys[y], xs[end], ys[y + 1]);
      return previous == Membership::kFull
                 ? append(result.interior_pixels, result.interior_count, run)
                 : append(result.fringe_pixels, result.fringe_count, run);
    };
    for (size_t x = 0; x + 1 < nx; x++) {
      const auto cell = IRect::MakeLTRB(xs[x], ys[y], xs[x + 1], ys[y + 1]);
      Membership membership = Membership::kFull;
      if (!ForEachNode([&](const auto& node) {
            const bool outside = !node.outer_pixels.IntersectsWithRect(cell);
            const bool full = node.HasFullCoverage(cell);
            if (node.operation == ClipOperation::kIntersect) {
              if (outside || membership == Membership::kOutside) {
                membership = Membership::kOutside;
              } else if (!full) {
                membership = Membership::kFringe;
              }
            } else if (full || membership == Membership::kOutside) {
              membership = Membership::kOutside;
            } else if (!outside) {
              membership = Membership::kFringe;
            }
            return true;
          })) {
        return std::nullopt;
      }
      if (membership != previous) {
        if (!publish(x)) {
          return conservative();
        }
        start = x;
        previous = membership;
      }
    }
    if (!publish(nx - 1)) {
      return conservative();
    }
  }
  return result;
}

AvioSample4ClipPool::AvioSample4ClipPool() {
  for (auto& slot : slots_) {
    slot = std::make_shared<AvioSample4ClipDescriptor>();
  }
}

std::shared_ptr<AvioSample4ClipDescriptor> AvioSample4ClipPool::Acquire(
    const std::shared_ptr<const AvioSample4ClipDescriptor>& previous) {
  if (previous &&
      (previous->GetDepth() == 0 ||
       previous->GetDepth() >= AvioSample4ClipDescriptor::kMaxDepth)) {
    return nullptr;
  }
  if (acquiring_.test_and_set(std::memory_order_acquire)) {
    return nullptr;
  }
  std::shared_ptr<AvioSample4ClipDescriptor> acquired;
  for (auto& slot : slots_) {
    if (slot.use_count() == 1) {
      // Only strong logical owners participate; there are no weak resurrected
      // readers after returning a slot. Acquire prior reader releases before
      // replacing its CPU data or mask leases.
      std::atomic_thread_fence(std::memory_order_acquire);
      acquired = slot;
      *acquired = previous ? *previous : AvioSample4ClipDescriptor{};
      if (previous && previous->count == AvioSample4ClipDescriptor::kCapacity) {
        *acquired = {};
        acquired->parent = previous;
      }
      break;
    }
  }
  acquiring_.clear(std::memory_order_release);
  return acquired;
}

bool AvioSample4ClipPool::ReclaimUnused() {
  if (acquiring_.test_and_set(std::memory_order_acquire)) {
    return false;
  }
  for (auto& slot : slots_) {
    if (slot.use_count() == 1) {
      std::atomic_thread_fence(std::memory_order_acquire);
      const auto* parent = slot->parent.get();
      *slot = {};
      // Returning a leaf can return earlier K4 bundles too. Release that
      // otherwise unread chain before the atlas admits its next writer.
      size_t remaining = slots_.size();
      while (parent && remaining--) {
        auto standing = std::find_if(
            slots_.begin(), slots_.end(),
            [&](const auto& entry) { return entry.get() == parent; });
        if (standing == slots_.end() || standing->use_count() != 1) {
          break;
        }
        const auto* previous = (*standing)->parent.get();
        **standing = {};
        parent = previous;
      }
    }
  }
  acquiring_.clear(std::memory_order_release);
  return true;
}

}  // namespace impeller
