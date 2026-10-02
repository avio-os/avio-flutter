// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/sample4_clip.h"

#include <cmath>
#include "gtest/gtest.h"

namespace impeller::testing {
namespace {
CoverageConvexQuad4 Quad() {
  return *CoverageConvexQuad4::Make(
      Rect::MakeXYWH(.25f, .25f, 8, 8).GetPoints());
}
}  // namespace

TEST(AvioSample4Clip, IntersectsLaneIdentityBeforeResolving) {
  AvioSample4ClipDescriptor descriptor;
  ASSERT_TRUE(descriptor.AppendQuad(Quad(), ClipOperation::kIntersect));
  ASSERT_TRUE(descriptor.AppendQuad(Quad(), ClipOperation::kDifference));
  for (ClipSampleMask4 geometry = 0; geometry < 16; geometry++) {
    for (ClipSampleMask4 clip = 0; clip < 16; clip++) {
      for (ClipSampleMask4 difference = 0; difference < 16; difference++) {
        const std::array<ClipSampleMask4, 2> masks = {clip, difference};
        EXPECT_EQ(descriptor.Combine(masks, geometry),
                  (geometry & clip & ~difference) & kClipSampleMask4Full);
      }
    }
  }
  // Identical half masks intersect to half coverage, not one quarter; disjoint
  // halves are empty, not the same quarter produced by alpha multiplication.
  const std::array<ClipSampleMask4, 2> masks = {3, 0};
  EXPECT_EQ(ResolveClipSampleMask4(*descriptor.Combine(masks, 3)), .5f);
  EXPECT_EQ(ResolveClipSampleMask4(*descriptor.Combine(masks, 12)), 0);
}

TEST(AvioSample4Clip, CapacityRefusalNeverReplacesTheParent) {
  AvioSample4ClipDescriptor descriptor;
  for (size_t i = 0; i < descriptor.kCapacity; i++) {
    ASSERT_TRUE(descriptor.AppendQuad(Quad(), ClipOperation::kIntersect));
  }
  EXPECT_FALSE(descriptor.AppendQuad(Quad(), ClipOperation::kDifference));
  EXPECT_EQ(descriptor.count, 4u);
  const std::array<ClipSampleMask4, 4> masks = {15, 7, 3, 1};
  EXPECT_EQ(descriptor.Combine(masks), 1);
  EXPECT_FALSE(descriptor.Combine(std::span(masks).first(3)));
  EXPECT_FALSE(descriptor.Combine(masks, 16));
  descriptor.nodes[0].quad.reset();
  EXPECT_FALSE(descriptor.Combine(masks));
}

TEST(AvioSample4Clip, HeldPacketStatesPinSlotsAcrossReuseAndContextDrop) {
  std::shared_ptr<const AvioSample4ClipDescriptor> packet;
  {
    AvioSample4ClipPool pool;
    std::array<std::shared_ptr<const AvioSample4ClipDescriptor>,
               AvioSample4ClipPool::kCapacity>
        readers;
    for (size_t i = 0; i < readers.size(); i++) {
      auto state = pool.Acquire(nullptr);
      ASSERT_TRUE(state);
      ASSERT_TRUE(state->AppendQuad(Quad(), ClipOperation::kIntersect));
      state->segment_token = i + 1;
      readers[i] = std::move(state);
    }
    EXPECT_FALSE(pool.Acquire(readers[0]));
    packet = readers[0];
    const auto returned = readers[7].get();
    readers[7].reset();
    auto successor = pool.Acquire(packet);
    ASSERT_TRUE(successor);
    EXPECT_EQ(successor.get(), returned);
    EXPECT_EQ(successor->count, 1u);
    successor->segment_token = 999;
    EXPECT_EQ(packet->segment_token, 1u);
  }
  ASSERT_TRUE(packet);
  const std::array<ClipSampleMask4, 1> mask = {7};
  EXPECT_EQ(packet->Combine(mask, 3), 3);
}

TEST(AvioSample4Clip, ReclaimsOnlyReturnedLogicalStates) {
  AvioSample4ClipPool pool;
  auto held = pool.Acquire(nullptr);
  ASSERT_TRUE(held);
  ASSERT_TRUE(held->AppendQuad(Quad(), ClipOperation::kIntersect));
  held->segment_token = 31;
  auto unused = pool.Acquire(held);
  ASSERT_TRUE(unused);
  const auto returned_slot = unused.get();
  unused.reset();
  ASSERT_TRUE(pool.ReclaimUnused());
  EXPECT_EQ(held->segment_token, 31u);
  EXPECT_EQ(held->count, 1u);
  // The pool still owns this physical CPU slot, so inspecting it does not
  // recreate a logical reader or permit weak-reference resurrection.
  EXPECT_EQ(returned_slot->count, 0u);
  auto successor = pool.Acquire(held);
  ASSERT_TRUE(successor);
  EXPECT_EQ(successor.get(), returned_slot);
  EXPECT_EQ(successor->segment_token, 31u);
}

TEST(AvioSample4Clip, FiveDeepBundlesRetainEveryAncestorAndOperation) {
  AvioSample4ClipPool pool;
  std::shared_ptr<const AvioSample4ClipDescriptor> current;
  for (uint64_t token = 1; token <= 9; token++) {
    auto next = pool.Acquire(current);
    ASSERT_TRUE(next);
    ASSERT_TRUE(next->AppendQuad(Quad(), token == 6
                                             ? ClipOperation::kDifference
                                             : ClipOperation::kIntersect));
    next->nodes[next->count - 1].declaration_token = token;
    next->logical_pass_size = ISize(1920, 1080);
    current = std::move(next);
  }
  ASSERT_EQ(current->GetDepth(), 9u);
  size_t index = 0;
  ASSERT_TRUE(current->ForEachNode([&](const auto& node) {
    EXPECT_EQ(node.declaration_token, ++index);
    return true;
  }));
  std::array<ClipSampleMask4, 9> masks;
  masks.fill(15);
  masks[5] = 3;
  EXPECT_EQ(current->Combine(masks), 12);
  size_t stop = 0;
  EXPECT_FALSE(current->ForEachNode([&](const auto&) { return ++stop < 5; }));
  EXPECT_EQ(stop, 5u);
  auto held_parent = current->parent;
  current.reset();
  ASSERT_TRUE(pool.ReclaimUnused());
  EXPECT_EQ(held_parent->GetDepth(), 8u);
  held_parent.reset();
  ASSERT_TRUE(pool.ReclaimUnused());
  // Releasing the leaf recursively returns otherwise unread bundles in the
  // same sweep; every simultaneous claim can reuse its standing slot.
  std::array<std::shared_ptr<AvioSample4ClipDescriptor>,
             AvioSample4ClipPool::kCapacity>
      readers;
  for (auto& reader : readers) {
    reader = pool.Acquire(nullptr);
    ASSERT_TRUE(reader);
  }
}

TEST(AvioSample4Clip, FringePartitionMatchesNestedNativeLaneMembership) {
  AvioSample4ClipPool pool;
  std::shared_ptr<const AvioSample4ClipDescriptor> descriptor;
  const std::array<Rect, 6> bounds = {
      Rect::MakeLTRB(.25, .5, 13.75, 13.5),
      Rect::MakeLTRB(1, 1, 13, 13),
      Rect::MakeLTRB(2.125, 2.375, 12.5, 12.625),
      Rect::MakeLTRB(3, 3, 12, 12),
      Rect::MakeLTRB(4, 4, 11, 11),
      Rect::MakeLTRB(6.25, 6.5, 8.75, 9.25)};
  for (size_t i = 0; i < bounds.size(); i++) {
    auto next = pool.Acquire(descriptor);
    ASSERT_TRUE(next);
    ASSERT_TRUE(next->AppendQuad(
        *CoverageConvexQuad4::Make(bounds[i].GetPoints()),
        i == 5 ? ClipOperation::kDifference : ClipOperation::kIntersect));
    descriptor = std::move(next);
  }
  auto plan = descriptor->GetFringePlan(IRect::MakeLTRB(0, 0, 15, 15));
  ASSERT_TRUE(plan);
  EXPECT_GT(plan->interior_count, 0u);
  EXPECT_GT(plan->fringe_count, 0u);
  for (int y = 0; y < 15; y++) {
    for (int x = 0; x < 15; x++) {
      const auto pixel = IRect::MakeXYWH(x, y, 1, 1);
      ClipSampleMask4 lanes = 15;
      for (size_t i = 0; i < bounds.size(); i++) {
        lanes = CombineClipSampleMask4(
            lanes, *ClipRectSampleMask4(bounds[i], Point(x, y)),
            i == 5 ? ClipOperation::kDifference : ClipOperation::kIntersect);
      }
      size_t interiors = 0, fringes = 0;
      for (size_t i = 0; i < plan->interior_count; i++) {
        interiors += plan->interior_pixels[i].Contains(pixel);
      }
      for (size_t i = 0; i < plan->fringe_count; i++) {
        fringes += plan->fringe_pixels[i].Contains(pixel);
      }
      EXPECT_LE(interiors + fringes, 1u);
      if (interiors) {
        EXPECT_EQ(lanes, 15);
      }
      if (lanes) {
        EXPECT_EQ(interiors + fringes, 1u);
      }
    }
  }
}

TEST(AvioSample4Clip, CurvedClipInteriorNeverUsesResolvedAlpha) {
  AvioSample4ClipDescriptor descriptor;
  const auto bounds = Rect::MakeXYWH(.25, .5, 40, 24);
  ASSERT_TRUE(
      descriptor.AppendQuad(*CoverageConvexQuad4::Make(bounds.GetPoints()),
                            ClipOperation::kIntersect));
  // Replace the rectangular proof with the native oval cardinal-vertex proof.
  PopulateSample4CoverageProof(descriptor.nodes[0], Matrix{},
                               AvioContinuousClip::OvalClip(bounds), {});
  auto plan = descriptor.GetFringePlan(IRect::MakeXYWH(0, 0, 42, 26));
  ASSERT_TRUE(plan);
  ASSERT_GT(plan->interior_count, 0u);
  EXPECT_GT(plan->fringe_count, 0u);
  const auto center = bounds.GetCenter();
  for (size_t i = 0; i < plan->interior_count; i++) {
    const auto box = plan->interior_pixels[i];
    for (int64_t y = box.GetTop(); y < box.GetBottom(); y++) {
      for (int64_t x = box.GetLeft(); x < box.GetRight(); x++) {
        for (const auto& sample : kClipSampleLocations4) {
          const auto p = Point(x, y) + sample - center;
          EXPECT_LT(std::abs(p.x) / 20.f + std::abs(p.y) / 12.f, 1.f);
        }
      }
    }
  }
  // A path with no certified native interior remains bounded mask fringe;
  // coverage zero or an arbitrary scalar alpha never upgrades it to full.
  PopulateSample4CoverageProof(descriptor.nodes[0], Matrix{}, std::nullopt, {});
  plan = descriptor.GetFringePlan(IRect::MakeXYWH(0, 0, 42, 26));
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->interior_count, 0u);
  EXPECT_EQ(plan->fringe_count, 1u);
  EXPECT_EQ(plan->fringe_pixels[0], IRect::RoundOut(bounds));
}

TEST(AvioSample4Clip, FragmentationOverflowRemainsOneJointMaskFringe) {
  AvioSample4ClipPool pool;
  std::shared_ptr<const AvioSample4ClipDescriptor> descriptor;
  for (int i = 0; i < 40; i++) {
    auto next = pool.Acquire(descriptor);
    ASSERT_TRUE(next);
    ASSERT_TRUE(next->AppendQuad(
        *CoverageConvexQuad4::Make(
            Rect::MakeXYWH(i + .25f, i + .25f, 1, 1).GetPoints()),
        ClipOperation::kDifference));
    descriptor = std::move(next);
  }
  const auto writable = IRect::MakeXYWH(0, 0, 64, 64);
  auto plan = descriptor->GetFringePlan(writable);
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->interior_count, 0u);
  EXPECT_EQ(plan->fringe_count, 1u);
  EXPECT_EQ(plan->fringe_pixels[0], writable);
}

TEST(AvioSample4Clip, InvalidCoverageProofCannotRetainAnOldFullCertificate) {
  AvioSample4ClipDescriptor descriptor;
  ASSERT_TRUE(descriptor.AppendQuad(Quad(), ClipOperation::kIntersect));
  ASSERT_TRUE(descriptor.GetFringePlan(IRect::MakeXYWH(0, 0, 10, 10)));
  const std::array<IRect, 1> wrong = {IRect::MakeXYWH(-100, -100, 2, 2)};
  EXPECT_FALSE(descriptor.nodes[0].SetCoverageProof(
      descriptor.nodes[0].GetBounds(), wrong));
  EXPECT_FALSE(descriptor.GetFringePlan(IRect::MakeXYWH(0, 0, 10, 10)));
}

TEST(AvioSample4Clip, QuadTranslationPreservesSamplePhaseAndEqualityOwnership) {
  const auto original = Quad();
  const auto shifted = original.Translated(Vector2(-248, 504));
  EXPECT_EQ(shifted.GetBounds(), original.GetBounds().Shift(-248, 504));
  for (int y = -1; y < 10; y++) {
    for (int x = -1; x < 10; x++) {
      EXPECT_EQ(original.GetSampleMask(Point(x, y)),
                shifted.GetSampleMask(Point(x - 248, y + 504)));
    }
  }
}

TEST(AvioSample4Clip, RoundedCornersLeaveOnlyBoundedCornerFringes) {
  AvioSample4ClipDescriptor descriptor;
  const auto bounds = Rect::MakeXYWH(0, 0, 80, 40);
  ASSERT_TRUE(
      descriptor.AppendQuad(*CoverageConvexQuad4::Make(bounds.GetPoints()),
                            ClipOperation::kIntersect));
  const std::array<IRect, 2> native_straight_regions = {
      IRect::MakeLTRB(10, 0, 70, 40), IRect::MakeLTRB(0, 10, 80, 30)};
  PopulateSample4CoverageProof(
      descriptor.nodes[0], Matrix{},
      AvioContinuousClip::RoundRectClip(RoundRect::MakeRectRadius(bounds, 10)),
      {.geometry = &native_straight_regions,
       .covers_area = [](const void* proof, const Matrix&, IRect pixels) {
         const auto& regions = *static_cast<const std::array<IRect, 2>*>(proof);
         return regions[0].Contains(pixels) || regions[1].Contains(pixels);
       }});
  auto plan = descriptor.GetFringePlan(IRect::MakeXYWH(0, 0, 80, 40));
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->fringe_count, 4u);
  int64_t fringe_area = 0;
  for (size_t i = 0; i < plan->fringe_count; i++) {
    fringe_area +=
        plan->fringe_pixels[i].GetWidth() * plan->fringe_pixels[i].GetHeight();
  }
  EXPECT_EQ(fringe_area, 400);
  int64_t interior_area = 0;
  for (size_t i = 0; i < plan->interior_count; i++) {
    interior_area += plan->interior_pixels[i].GetWidth() *
                     plan->interior_pixels[i].GetHeight();
  }
  EXPECT_EQ(interior_area + fringe_area, 3200);
}

TEST(AvioSample4Clip,
     SuperellipseAndPathUseOnlyNativeOwnerInteriorCertificates) {
  const auto bounds = Rect::MakeXYWH(0, 0, 80, 40);
  for (bool superellipse : {false, true}) {
    AvioSample4ClipDescriptor descriptor;
    ASSERT_TRUE(
        descriptor.AppendQuad(*CoverageConvexQuad4::Make(bounds.GetPoints()),
                              ClipOperation::kIntersect));
    const auto native_inner = IRect::MakeLTRB(10, 8, 70, 32);
    PopulateSample4CoverageProof(
        descriptor.nodes[0], Matrix{},
        superellipse ? std::optional(AvioContinuousClip::SuperellipseClip(
                           RoundSuperellipse::MakeRectRadius(bounds, 24)))
                     : std::nullopt,
        {.geometry = &native_inner,
         .covers_area = [](const void* proof, const Matrix&, IRect pixels) {
           return static_cast<const IRect*>(proof)->Contains(pixels);
         }});
    auto plan = descriptor.GetFringePlan(IRect::MakeXYWH(0, 0, 80, 40));
    ASSERT_TRUE(plan);
    ASSERT_GT(plan->interior_count, 0u);
    ASSERT_GT(plan->fringe_count, 0u);
    for (size_t i = 0; i < plan->interior_count; i++) {
      EXPECT_TRUE(native_inner.Contains(plan->interior_pixels[i]));
    }
  }
}

TEST(AvioSample4Clip, Exact512NodeLimitKeepsHeldRecipeImmutableAndRejects513) {
  AvioSample4ClipPool pool;
  std::shared_ptr<const AvioSample4ClipDescriptor> current;
  for (size_t i = 0; i < AvioSample4ClipDescriptor::kMaxDepth; i++) {
    auto next = pool.Acquire(current);
    ASSERT_TRUE(next) << i;
    ASSERT_TRUE(next->AppendQuad(Quad(), ClipOperation::kIntersect));
    next->nodes[next->count - 1].declaration_token = i + 1;
    current = std::move(next);
  }
  ASSERT_EQ(current->GetDepth(), 512u);
  auto held = current;
  EXPECT_FALSE(pool.Acquire(current));
  EXPECT_EQ(held->GetDepth(), 512u);
  std::array<ClipSampleMask4, 512> masks;
  masks.fill(15);
  masks[0] = 7;
  masks[511] = 3;
  EXPECT_EQ(held->Combine(masks), 3);
  // The held recipe owns exactly 128 bundles. Only the append spare can be
  // lent to a new logical stack; no capacity growth may overwrite old readers.
  auto spare = pool.Acquire(nullptr);
  ASSERT_TRUE(spare);
  EXPECT_FALSE(pool.Acquire(nullptr));
  current.reset();
  ASSERT_TRUE(pool.ReclaimUnused());
  EXPECT_EQ(held->GetDepth(), 512u);
  held.reset();
  spare.reset();
  ASSERT_TRUE(pool.ReclaimUnused());
  std::array<std::shared_ptr<AvioSample4ClipDescriptor>,
             AvioSample4ClipPool::kCapacity>
      all;
  for (auto& owner : all) {
    owner = pool.Acquire(nullptr);
    ASSERT_TRUE(owner);
  }
  EXPECT_FALSE(pool.Acquire(nullptr));
}

}  // namespace impeller::testing
