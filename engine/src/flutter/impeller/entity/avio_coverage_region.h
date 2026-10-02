// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_AVIO_COVERAGE_REGION_H_
#define FLUTTER_IMPELLER_ENTITY_AVIO_COVERAGE_REGION_H_

#include <array>
#include <functional>
#include <memory>
#include <string>

#include "impeller/core/allocator.h"
#include "impeller/entity/avio_region_packing.h"
#include "impeller/renderer/render_resource_report.h"
#include "impeller/renderer/render_resource_usage.h"
#include "impeller/renderer/render_target.h"

namespace impeller {

template <class T>
struct AvioCoverageLeaseAllocator;

// Negotiation supplies budgets before creation; no frame grows a warm region.
struct AvioCoverageRegionConfig {
  ISize coverage_size = {1024, 512};
  ISize colour_size = {256, 256};
  // Distinct images prevent framebuffer feedback when a parent samples a
  // nested layer. No two live layer leases share a texture, even at disjoint
  // rectangles. The measured sum of these fixed warm images is capped below.
  std::array<ISize, 5> layer_sizes = {ISize{768, 768}, ISize{512, 512},
                                      ISize{256, 256}, ISize{256, 256},
                                      ISize{128, 128}};
  PixelFormat colour_format = PixelFormat::kB8G8R8A8UNormInt;
  // Atlas winding uses S8 only. The bounded colour island may need depth.
  PixelFormat colour_depth_stencil_format = PixelFormat::kD32FloatS8UInt;
  size_t coverage_max_bytes = 8u * 1024u * 1024u;
  size_t layer_max_bytes = 4u * 1024u * 1024u;
  // Vulkan caches sampled native masks. GLES replays original winding/clip
  // geometry in the native colour tiles, without a sampler2DMS mask cache.
  bool cache_native_masks = true;
  // Vulkan proves actual allocation requirements. GLES has no portable
  // physical-byte query; its opt-in cap proves descriptor bytes only and
  // leaves actual-byte fields explicitly unsupported in the report.
  bool require_exact_allocated_bytes = true;
  // Optional continuous coverage reads the exact native sample prefix from a
  // distinct fixed image. It is part of this coverage bank, never a frame
  // allocation or a resolved single-sample substitute.
  bool continuous_destination_prefix = false;
};

struct AvioCoverageRegionUsage {
  RenderResourceUsage coverage;
  RenderResourceUsage layers;
  // One flush is one successfully encoded colour-island tile composite back
  // to its parent. Direct 1x draws, failed copies and atlas resets do not
  // count.
  uint64_t coverage_flushes = 0u;
  // Raster-thread texture allocations are layer overflow.
  uint64_t layer_region_overflow = 0u;
  uint64_t layer_region_overflow_real_bytes = 0u;
};

// The single graphics queue orders atlas writes after the previous batch's
// encoded readers (EN46a). Logical leases prevent reuse before those readers
// have been encoded. RenderPass/CommandBuffer track the attached textures
// independently through actual GPU completion; a CPU release is no GPU fence.
class AvioCoverageRegion final {
 private:
  struct State;

 public:
  // Each coverage packer and the distinct-layer bank has at most 512 claims.
  // Expired weak references retain their control slot until they are dropped;
  // bounded exhaustion cannot resurrect an old allocation's weak identity.
  static constexpr size_t kMaximumLeaseClaims =
      3u * AvioRegionPacking::kMaximumAllocations;
  enum class Kind { kMask, kColour, kLayer };
  enum class Status {
    kSuccess,
    kInvalid,
    kFrameNotOpen,
    kNotInitialized,
    kNeedsFlush,
    kNeedsTiling,
    kAllocationFailed,
  };

  class Lease final {
   public:
    ~Lease();
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    // Always returns the physical page extent. The content rectangle, UVs and
    // MVP translation remain explicit; a page never pretends to be a subimage.
    RenderTarget GetRenderTarget() const;
    IRect GetRenderArea() const { return area_; }
    IRect GetContentRect() const { return content_; }
    ISize GetRequestedSize() const { return content_.GetSize(); }
    Kind GetKind() const { return kind_; }
    bool IsValid() const;
    bool IsOverflow() const { return overflow_; }
    void Release();

   private:
    friend class AvioCoverageRegion;
    template <class T>
    friend struct AvioCoverageLeaseAllocator;
    Lease(std::shared_ptr<State> state,
          Kind kind,
          uint64_t frame,
          AvioRegionPacking::Allocation allocation,
          bool overflow = false,
          bool clip_mask_scratch = false);
    std::weak_ptr<State> state_;
    Kind kind_;
    uint64_t frame_;
    AvioRegionPacking::Token token_;
    IRect area_;
    IRect content_;
    bool overflow_ = false;
    bool released_ = false;
    bool clip_mask_scratch_ = false;
  };

  struct Acquisition {
    Status status = Status::kInvalid;
    std::shared_ptr<Lease> lease;
  };

  // Creates every warm texture once. Measured Vulkan requirements, including
  // D32S8 padding, must fit the negotiated budgets or initialization fails.
  static std::shared_ptr<AvioCoverageRegion> Create(
      std::shared_ptr<Allocator> allocator,
      const AvioCoverageRegionConfig& config,
      std::string* error = nullptr);

  // Cold full-attachment clears establish initialized contents before any
  // partial-area rendering preserves the rest. A callback must encode the
  // full clear, required shader-read transition, and ordered queue enqueue.
  // Failed initialization is sticky; no frame path retries provisioning.
  bool InitializeMaskOnce(const std::function<bool()>& initialize);
  bool InitializeColourOnce(const std::function<bool()>& initialize);
  bool IsMaskInitialized() const;
  bool IsColourInitialized() const;

  bool BeginRasterFrame(uint64_t frame_epoch);
  // Ends colour lifetimes. Live masks and layers remain immutable while a
  // cache, snapshot or logical clip reader owns them across raster frames.
  void EndRasterFrame();
  Acquisition AcquireCoverage(Kind kind, ISize size);
  // Exclusive temporary native mask, released after its consumer is encoded.
  // The entire physical scratch is cleared before replay; only the requested
  // origin-zero content is sampled. No persistent recipe/cache may overwrite
  // it while an older logical consumer retains the exact claim.
  Acquisition AcquireClipMaskScratch(ISize size);
  // A distinct warm 1x/no-D/S image. Incompatible/mip/oversized requests are
  // explicitly counted frame-local 1x overflow, never full-size MSAA fallback.
  Acquisition AcquireLayer(ISize size,
                           int32_t mip_count = 1,
                           bool exact_extent = false);
  // The callback encodes current readers and drops discardable cache refs.
  // Surviving clip/cache leases veto reuse, including across frame boundaries.
  bool FlushCoverageBatch(const std::function<bool()>& encode_and_composite);
  // Called only after the tiled executor successfully encodes CopyTile back
  // to the parent. Counts that composite without resetting any mask packing or
  // changing logical readers. Encoding success is not a GPU completion fence.
  void RecordColourIslandComposite();

  // Raw fixed physical targets for the central tiled executor. That owner
  // must finish/seed/render/composite each tile in queue order before reuse.
  RenderTarget GetColourIslandTarget() const;
  RenderTarget GetCoverageAtlasTarget() const;
  // Fixed native R8 sample lanes for deep clip replay. Its D/S attachments
  // borrow the colour island, so the owner must finish colour work before
  // clearing/replaying this full target, then encode its consumer before
  // releasing the exclusive scratch lease. It has no persistent readers.
  // Empty when the backend does not cache sampled native masks.
  RenderTarget GetClipMaskScratchTarget() const;
  // GLES FBOs are not shader textures. This distinct warm 1x image receives
  // the exact parent prefix before native tile seeding; never a frame
  // allocation.
  std::shared_ptr<Texture> GetColourSeedTexture() const;
  std::shared_ptr<Texture> GetContinuousDestinationPrefix() const;
  std::array<RenderTarget, 5> GetWarmLayerTargets() const;
  ISize GetColourIslandSize() const;
  ISize GetCoverageAtlasSize() const;
  ISize GetCoverageTileSize(Kind kind) const;
  const AvioCoverageRegionConfig& GetConfig() const;
  AvioCoverageRegionUsage ReportUsage(bool start_new_interval);
  bool HasExactAllocatedBytes() const;
  bool CachesNativeMasks() const;
  AvioRenderResourceReport GetDescriptorResourceReport(
      bool start_new_interval = false) const;
  // All-hidden trims preserve both context-owned warm regions.
  void TrimIdleResourceCaches() {}

 private:
  explicit AvioCoverageRegion(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;
};

}  // namespace impeller
#endif  // FLUTTER_IMPELLER_ENTITY_AVIO_COVERAGE_REGION_H_
