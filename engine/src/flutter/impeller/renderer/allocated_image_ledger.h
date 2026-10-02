// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_ALLOCATED_IMAGE_LEDGER_H_
#define FLUTTER_IMPELLER_RENDERER_ALLOCATED_IMAGE_LEDGER_H_

#include <array>
#include <memory>
#include <mutex>

#include "impeller/renderer/render_resource_report.h"
#include "impeller/renderer/render_resource_scope.h"

namespace impeller {

// An explicit descriptor identity, independent of renderer/Vulkan headers.
// The allocator fills every descriptor field; equality never relies on a hash.
struct AvioAllocatedImageKey {
  std::array<uint64_t, 12> fields = {};
  constexpr bool operator==(const AvioAllocatedImageKey&) const = default;
};

// Physical allocated-image census. Registration is stored with the actual
// deferred image resource, not the Texture wrapper. Imported images never
// register. Intrusive records add no heap allocation at image creation/report.
// Lease/idleness fields stay zero: the census does not infer GPU completion or
// logical cache leases from physical memory ownership.
class AllocatedImageLedger final
    : public std::enable_shared_from_this<AllocatedImageLedger> {
 public:
  class Registration final {
   public:
    Registration() = default;
    ~Registration();
    Registration(Registration&& other) noexcept;
    Registration& operator=(Registration&& other) noexcept;
    Registration(const Registration&) = delete;
    Registration& operator=(const Registration&) = delete;
    // Called only after a successfully encoded buffer-to-texture upload.
    // Explicit class and raster origin exclude imports and scratch targets.
    void RecordImageUpload(bool raster_frame) const;

   private:
    friend class AllocatedImageLedger;
    Registration(std::shared_ptr<AllocatedImageLedger> ledger,
                 AvioRenderResourceKind kind,
                 AvioAllocatedImageKey key,
                 size_t nominal_bytes,
                 size_t real_bytes,
                 bool raster_frame,
                 AvioRasterAllocationCause cause);
    void Reset();
    std::shared_ptr<AllocatedImageLedger> ledger_;
    AvioRenderResourceKind kind_ = AvioRenderResourceKind::kImageTextures;
    AvioAllocatedImageKey key_;
    size_t nominal_ = 0u;
    size_t real_ = 0u;
    Registration* previous_ = nullptr;
    Registration* next_ = nullptr;
  };

  Registration Register(
      AvioRenderResourceKind kind,
      AvioAllocatedImageKey key,
      size_t nominal_bytes,
      size_t real_bytes,
      bool raster_frame,
      AvioRasterAllocationCause cause = AvioRasterAllocationCause::kFrameWork);
  AvioRenderResourceReport Report(bool start_new_interval);

 private:
  static constexpr size_t kKindCount = 8u;
  void Add(Registration& registration,
           bool raster_frame,
           AvioRasterAllocationCause cause);
  void Remove(Registration& registration);
  void Replace(Registration& from, Registration& to);
  void RecordImageUpload(AvioRenderResourceKind kind, bool raster_frame);
  static size_t KindIndex(AvioRenderResourceKind kind);
  std::mutex mutex_;
  Registration* first_ = nullptr;
  std::array<RenderResourceUsage, kKindCount> usage_ = {};
  std::array<bool, kKindCount> observed_ = {};
  uint64_t raster_allocations_ = 0u;
  uint64_t snapshot_allocations_ = 0u;
  uint64_t image_uploads_ = 0u;
  uint64_t glyph_atlas_growths_ = 0u;
};

}  // namespace impeller
#endif  // FLUTTER_IMPELLER_RENDERER_ALLOCATED_IMAGE_LEDGER_H_
