// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/avio_coverage_region.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

#include "fml/logging.h"
#include "impeller/renderer/render_resource_scope.h"

namespace impeller {
namespace {

IRect ToRect(AvioRegionPacking::Rect rect) {
  return IRect::MakeXYWH(rect.x, rect.y, rect.width, rect.height);
}

bool Fits(ISize size, ISize maximum) {
  return size.width > 0 && size.height > 0 && size.width <= maximum.width &&
         size.height <= maximum.height;
}

TextureDescriptor Descriptor(ISize size,
                             PixelFormat format,
                             bool multisample,
                             size_t mip_count = 1u) {
  TextureDescriptor desc;
  desc.storage_mode = StorageMode::kDevicePrivate;
  desc.type = multisample ? TextureType::kTexture2DMultisample
                          : TextureType::kTexture2D;
  desc.sample_count = multisample ? SampleCount::kCount4 : SampleCount::kCount1;
  desc.format = format;
  desc.size = size;
  desc.mip_count = mip_count;
  desc.usage = TextureUsage::kRenderTarget;
  // Native four-sample R8 masks preserve sample identity for intersections;
  // the 1x resolve alone cannot represent their membership bits.
  if ((!multisample && format != PixelFormat::kS8UInt) ||
      (multisample && format == PixelFormat::kR8UNormInt)) {
    desc.usage |= TextureUsage::kShaderRead;
  }
  return desc;
}

}  // namespace

// A claim gets a new shared control block and generation from cold storage.
// Unlike a standing strong shared_ptr<Lease>, expired weak readers can never
// lock a newer lease. Their bounded slots remain occupied until final weak
// destruction. The allocator itself keeps this storage alive after region
// teardown, without owning State or a standing Lease reference.
struct AvioCoverageLeaseStorage {
  static constexpr size_t kSlotBytes = sizeof(AvioCoverageRegion::Lease) + 128;
  struct Slot {
    alignas(std::max_align_t) std::array<std::byte, kSlotBytes> bytes;
    std::atomic_bool occupied = false;
  };
  std::array<Slot, AvioCoverageRegion::kMaximumLeaseClaims> slots;
  size_t cursor = 0;

  std::optional<size_t> Reserve() {
    for (size_t i = 0; i < slots.size(); i++) {
      const size_t index = (cursor + i) % slots.size();
      bool vacant = false;
      if (slots[index].occupied.compare_exchange_strong(
              vacant, true, std::memory_order_acquire)) {
        cursor = (index + 1) % slots.size();
        return index;
      }
    }
    return std::nullopt;
  }
  void Release(size_t index) {
    slots[index].occupied.store(false, std::memory_order_release);
  }
};

template <class T>
struct AvioCoverageLeaseAllocator {
  using value_type = T;
  std::shared_ptr<AvioCoverageLeaseStorage> storage;
  size_t index;

  AvioCoverageLeaseAllocator(std::shared_ptr<AvioCoverageLeaseStorage> storage,
                             size_t index)
      : storage(std::move(storage)), index(index) {}
  template <class U>
  AvioCoverageLeaseAllocator(const AvioCoverageLeaseAllocator<U>& other)
      : storage(other.storage), index(other.index) {}

  T* allocate(size_t count) noexcept {
    // Rebound shared-control-block layout is proved by the actual standard
    // library compilation. Reserve happened before any packing/GPU mutation;
    // this cannot throw on the engine's -fno-exceptions raster path.
    static_assert(sizeof(T) <= AvioCoverageLeaseStorage::kSlotBytes);
    static_assert(alignof(T) <= alignof(std::max_align_t));
    FML_CHECK(count == 1 && storage->slots[index].occupied.load());
    return reinterpret_cast<T*>(storage->slots[index].bytes.data());
  }
  void deallocate(T*, size_t) noexcept { storage->Release(index); }
  template <class U, class... Args>
  void construct(U* pointer, Args&&... args) {
    ::new (static_cast<void*>(pointer)) U(std::forward<Args>(args)...);
  }
  template <class U>
  bool operator==(const AvioCoverageLeaseAllocator<U>& other) const {
    return storage == other.storage && index == other.index;
  }
};

struct AvioCoverageRegion::State {
  struct LeaseReservation {
    std::shared_ptr<AvioCoverageLeaseStorage> storage;
    size_t index;
    bool committed = false;
    LeaseReservation(std::shared_ptr<AvioCoverageLeaseStorage> storage,
                     size_t index)
        : storage(std::move(storage)), index(index) {}
    LeaseReservation(const LeaseReservation&) = delete;
    LeaseReservation(LeaseReservation&& other)
        : storage(std::move(other.storage)),
          index(other.index),
          committed(other.committed) {
      other.committed = true;
    }
    ~LeaseReservation() {
      if (!committed)
        storage->Release(index);
    }
    std::shared_ptr<Lease> Create(std::shared_ptr<State> state,
                                  Kind kind,
                                  uint64_t frame,
                                  AvioRegionPacking::Allocation allocation,
                                  bool overflow = false,
                                  bool clip_mask_scratch = false) {
      auto result = std::allocate_shared<Lease>(
          AvioCoverageLeaseAllocator<Lease>(storage, index), std::move(state),
          kind, frame, allocation, overflow, clip_mask_scratch);
      committed = true;
      return result;
    }
  };
  struct Resource {
    std::shared_ptr<Texture> texture;
    size_t nominal = 0u;
    size_t real = 0u;
    // The target and this record's references, excluding command readers.
    size_t owned_references = 2u;
  };
  struct Layer {
    RenderTarget target;
    Resource resource;
    uint64_t serial = 0u;
    uint64_t epoch = 0u;
    bool live = false;
    bool overflow = false;
  };

  explicit State(std::shared_ptr<Allocator> allocator,
                 AvioCoverageRegionConfig config)
      : allocator(std::move(allocator)),
        config(std::move(config)),
        mask_packing(this->config.coverage_size.width,
                     this->config.coverage_size.height),
        colour_packing(this->config.colour_size.width,
                       this->config.colour_size.height) {}

  std::shared_ptr<Allocator> allocator;
  std::shared_ptr<AvioCoverageLeaseStorage> lease_storage =
      std::make_shared<AvioCoverageLeaseStorage>();
  AvioCoverageRegionConfig config;
  RenderTarget mask;
  RenderTarget colour;
  RenderTarget clip_mask_scratch;
  std::shared_ptr<Texture> colour_seed;
  std::shared_ptr<Texture> continuous_destination_prefix;
  AvioRegionPacking mask_packing;
  AvioRegionPacking colour_packing;
  std::array<Resource, 7> coverage_resources = {};
  size_t coverage_count = 0u;
  size_t mask_resource_count = 0u;
  size_t clip_mask_resource = 0u;
  bool clip_mask_live = false;
  uint64_t clip_mask_serial = 0u;
  uint64_t clip_mask_frame = 0u;
  std::array<Layer, AvioRegionPacking::kMaximumAllocations> layers = {};
  size_t warm_layers = 0u;
  size_t layer_count = 0u;
  uint64_t next_serial = 1u;
  uint64_t frame = 0u;
  uint64_t last_frame = 0u;
  bool frame_open = false;
  std::once_flag mask_initialize_once;
  std::once_flag colour_initialize_once;
  std::atomic_bool mask_initialized = false;
  std::atomic_bool colour_initialized = false;
  AvioCoverageRegionUsage interval;

  // Final lease owners may leave on the native completion thread. They only
  // publish a serial to these fixed mailboxes; all packing, textures and census
  // state remain confined to the raster owner. Serial-max publication also
  // prevents a delayed old colour generation from hiding a newer return.
  using Returns =
      std::array<std::atomic_uint64_t, AvioRegionPacking::kMaximumAllocations>;
  Returns mask_returns{};
  Returns colour_returns{};
  Returns layer_returns{};
  std::atomic_uint64_t clip_mask_return{0};
  std::array<AvioRegionPacking::Token, AvioRegionPacking::kMaximumAllocations>
      mask_tokens{};
  std::array<AvioRegionPacking::Token, AvioRegionPacking::kMaximumAllocations>
      colour_tokens{};

  static void PublishReturn(std::atomic_uint64_t& mailbox, uint64_t serial) {
    auto previous = mailbox.load(std::memory_order_relaxed);
    while (previous < serial && !mailbox.compare_exchange_weak(
                                    previous, serial, std::memory_order_release,
                                    std::memory_order_relaxed)) {
    }
  }

  void Return(Kind kind, AvioRegionPacking::Token token, bool scratch) {
    if (scratch) {
      PublishReturn(clip_mask_return, token.serial);
    } else if (token.index < AvioRegionPacking::kMaximumAllocations) {
      auto& returns = kind == Kind::kMask     ? mask_returns
                      : kind == Kind::kColour ? colour_returns
                                              : layer_returns;
      PublishReturn(returns[token.index], token.serial);
    }
  }

  void DrainReturns() {
    for (size_t i = 0; i < AvioRegionPacking::kMaximumAllocations; ++i) {
      const auto mask = mask_returns[i].exchange(0, std::memory_order_acquire);
      if (mask != 0 && mask == mask_tokens[i].serial) {
        Release(Kind::kMask, 0, mask_tokens[i]);
      }
      const auto colour =
          colour_returns[i].exchange(0, std::memory_order_acquire);
      if (colour != 0 && colour == colour_tokens[i].serial) {
        Release(Kind::kColour, frame, colour_tokens[i]);
      }
      const auto layer =
          layer_returns[i].exchange(0, std::memory_order_acquire);
      if (layer != 0 && i < layer_count && layer == layers[i].serial) {
        Release(Kind::kLayer, layers[i].epoch, {0, i, layer});
      }
    }
    const auto scratch =
        clip_mask_return.exchange(0, std::memory_order_acquire);
    if (scratch != 0 && scratch == clip_mask_serial) {
      clip_mask_live = false;
    }
  }

  std::optional<LeaseReservation> ReserveLease() {
    auto index = lease_storage->Reserve();
    if (!index)
      return std::nullopt;
    return LeaseReservation(lease_storage, *index);
  }

  bool IsLive(Kind kind, uint64_t epoch, AvioRegionPacking::Token token) const {
    if (kind == Kind::kMask) {
      // Static mask caches and logical clips may retain a mask across frames.
      return mask_packing.IsLive(token);
    }
    if (kind == Kind::kColour) {
      return frame_open && epoch == frame && colour_packing.IsLive(token);
    }
    return token.index < layer_count && layers[token.index].live &&
           layers[token.index].serial == token.serial &&
           layers[token.index].epoch == epoch;
  }
  bool IsClipMaskLive(uint64_t epoch, AvioRegionPacking::Token token) const {
    return clip_mask_live && clip_mask_frame == epoch &&
           clip_mask_serial == token.serial;
  }

  void Release(Kind kind, uint64_t epoch, AvioRegionPacking::Token token) {
    if (!IsLive(kind, epoch, token)) {
      return;
    }
    if (kind == Kind::kMask) {
      mask_packing.Release(token);
    } else if (kind == Kind::kColour) {
      colour_packing.Release(token);
    } else {
      auto& layer = layers[token.index];
      layer.live = false;
      if (layer.overflow) {
        interval.layers.orphans_released_entries++;
        interval.layers.orphans_released_real_bytes += layer.resource.real;
        // Frame-local overflow has no cache owner. Its actual GPU readers may
        // still retain TextureSourceVK after the final logical reader drops.
        layer = {};
      }
    }
  }

  RenderTarget Target(Kind kind, AvioRegionPacking::Token token) const {
    if (kind == Kind::kMask) {
      return mask;
    }
    if (kind == Kind::kColour) {
      return colour;
    }
    return layers[token.index].target;
  }

  static void AddUsage(RenderResourceUsage& usage,
                       const Resource& resource,
                       bool logical_reader) {
    if (!resource.texture) {
      return;
    }
    usage.entries++;
    usage.nominal_bytes += resource.nominal;
    usage.real_bytes += resource.real;
    if (logical_reader || static_cast<size_t>(resource.texture.use_count()) >
                              resource.owned_references) {
      usage.leased_entries++;
      usage.peak_leased_nominal_bytes += resource.nominal;
    }
  }

  bool CoverageLogicalReader(size_t index) const {
    if (config.cache_native_masks && index == clip_mask_resource) {
      return clip_mask_live;
    }
    if (index < mask_resource_count) {
      return mask_packing.LiveCount() > 0u;
    }
    const auto stencil = colour.GetStencilAttachment();
    return colour_packing.LiveCount() > 0u ||
           (clip_mask_live && stencil &&
            coverage_resources[index].texture == stencil->texture);
  }

  AvioCoverageRegionUsage CurrentUsage() const {
    AvioCoverageRegionUsage result = interval;
    auto& coverage = result.coverage;
    auto& layer_usage = result.layers;
    coverage.entries = coverage.nominal_bytes = coverage.real_bytes = 0u;
    coverage.leased_entries = coverage.peak_leased_nominal_bytes = 0u;
    layer_usage.entries = layer_usage.nominal_bytes = layer_usage.real_bytes =
        0u;
    layer_usage.leased_entries = layer_usage.peak_leased_nominal_bytes = 0u;
    for (size_t i = 0; i < coverage_count; i++) {
      AddUsage(coverage, coverage_resources[i], CoverageLogicalReader(i));
    }
    for (size_t i = 0; i < layer_count; i++) {
      AddUsage(layer_usage, layers[i].resource, layers[i].live);
    }
    coverage.distinct_keys = coverage.entries;
    layer_usage.distinct_keys = 0u;
    for (size_t i = 0; i < layer_count; i++) {
      const auto& texture = layers[i].resource.texture;
      if (!texture) {
        continue;
      }
      bool duplicate = false;
      for (size_t j = 0; j < i; j++) {
        const auto& other = layers[j].resource.texture;
        if (other &&
            other->GetTextureDescriptor() == texture->GetTextureDescriptor()) {
          duplicate = true;
          break;
        }
      }
      layer_usage.distinct_keys += !duplicate;
    }
    layer_usage.duplicate_entries =
        layer_usage.entries - layer_usage.distinct_keys;
    coverage.peak_leased_nominal_bytes =
        std::max(coverage.peak_leased_nominal_bytes,
                 interval.coverage.peak_leased_nominal_bytes);
    layer_usage.peak_leased_nominal_bytes =
        std::max(layer_usage.peak_leased_nominal_bytes,
                 interval.layers.peak_leased_nominal_bytes);
    return result;
  }

  void UpdatePeak() {
    RenderResourceUsage coverage;
    RenderResourceUsage layer_usage;
    for (size_t i = 0; i < coverage_count; i++) {
      AddUsage(coverage, coverage_resources[i], CoverageLogicalReader(i));
    }
    for (size_t i = 0; i < layer_count; i++) {
      AddUsage(layer_usage, layers[i].resource, layers[i].live);
    }
    interval.coverage.peak_leased_nominal_bytes =
        std::max(interval.coverage.peak_leased_nominal_bytes,
                 coverage.peak_leased_nominal_bytes);
    interval.layers.peak_leased_nominal_bytes =
        std::max(interval.layers.peak_leased_nominal_bytes,
                 layer_usage.peak_leased_nominal_bytes);
  }

  std::optional<Resource> Allocate(TextureDescriptor desc,
                                   std::string_view label,
                                   size_t remaining_nominal,
                                   size_t remaining_real,
                                   std::string* error) {
    const auto fail = [&](const char* message) -> std::optional<Resource> {
      if (error) {
        *error = message;
      }
      return std::nullopt;
    };
    if (!Fits(desc.size, allocator->GetMaxTextureSizeSupported()) ||
        !desc.IsValid()) {
      return fail("Invalid or unsupported region texture descriptor.");
    }
    const size_t bytes = desc.GetByteSizeOfAllMipLevels();
    const size_t samples = static_cast<size_t>(desc.sample_count);
    if (bytes == 0u || bytes > remaining_nominal / samples) {
      return fail("Nominal region attachment bytes exceed the negotiated cap.");
    }
    auto texture = allocator->CreateTexture(desc);
    if (!texture || !texture->IsValid()) {
      return fail("Could not allocate a required warm region attachment.");
    }
    texture->SetLabel(label);
    const size_t real = config.require_exact_allocated_bytes
                            ? texture->GetAllocatedByteSize()
                            : 0u;
    // Exact Vulkan requirements must fit. Descriptor-only GLES mode keeps
    // unknown physical bytes unavailable; the nominal cap above still bounds
    // renderer-created samples, formats, extents and mip levels.
    if (config.require_exact_allocated_bytes &&
        (real == 0u || real > remaining_real)) {
      return fail(
          "Actual region attachment requirements exceed the cap or are "
          "unknown.");
    }
    return Resource{std::move(texture), bytes * samples, real, 2u};
  }

  bool CreateCoverageTarget(RenderTarget& target,
                            ISize size,
                            PixelFormat format,
                            PixelFormat stencil_format,
                            bool resolve_colour,
                            std::string* error) {
    const AvioResourceAllocationScope kind_scope(
        AvioRenderResourceKind::kCoverageRegion);
    size_t nominal = 0u;
    size_t real = 0u;
    for (size_t i = 0; i < coverage_count; i++) {
      nominal += coverage_resources[i].nominal;
      real += coverage_resources[i].real;
    }
    const auto allocate = [&](PixelFormat pixel_format, bool msaa) {
      if (coverage_count == coverage_resources.size()) {
        return std::optional<Resource>{};
      }
      auto result =
          Allocate(Descriptor(size, pixel_format, msaa), "Avio Coverage Region",
                   config.coverage_max_bytes - nominal,
                   config.coverage_max_bytes - real, error);
      if (result) {
        nominal += result->nominal;
        real += result->real;
        interval.coverage.created_entries++;
        interval.coverage.created_real_bytes += result->real;
        coverage_resources[coverage_count++] = *result;
      }
      return result;
    };
    auto msaa = allocate(format, true);
    auto resolve =
        msaa && resolve_colour ? allocate(format, false) : std::nullopt;
    auto stencil = msaa && (!resolve_colour || resolve)
                       ? allocate(stencil_format, true)
                       : std::nullopt;
    if (!stencil) {
      return false;
    }
    ColorAttachment color;
    color.texture = msaa->texture;
    color.resolve_texture = resolve ? resolve->texture : nullptr;
    color.load_action = LoadAction::kClear;
    color.store_action =
        resolve_colour ? StoreAction::kMultisampleResolve : StoreAction::kStore;
    target.SetColorAttachment(color, 0u);
    StencilAttachment stencil_attachment;
    stencil_attachment.texture = stencil->texture;
    stencil_attachment.load_action = LoadAction::kClear;
    stencil_attachment.store_action = StoreAction::kDontCare;
    target.SetStencilAttachment(stencil_attachment);
    if (stencil_format != PixelFormat::kS8UInt) {
      DepthAttachment depth;
      depth.texture = stencil->texture;
      depth.load_action = LoadAction::kClear;
      depth.store_action = StoreAction::kDontCare;
      target.SetDepthAttachment(depth);
      coverage_resources[coverage_count - 1u].owned_references = 3u;
    }
    return target.IsValid();
  }

  bool CreateClipMaskScratch(std::string* error) {
    size_t nominal = 0, real = 0;
    for (size_t i = 0; i < coverage_count; ++i) {
      nominal += coverage_resources[i].nominal;
      real += coverage_resources[i].real;
    }
    if (coverage_count == coverage_resources.size()) {
      return false;
    }
    const AvioResourceAllocationScope kind(
        AvioRenderResourceKind::kCoverageRegion);
    auto resource = Allocate(
        Descriptor(config.colour_size, PixelFormat::kR8UNormInt, true),
        "Avio Clip Sample Lanes Scratch", config.coverage_max_bytes - nominal,
        config.coverage_max_bytes - real, error);
    if (!resource) {
      return false;
    }
    ColorAttachment mask;
    mask.texture = resource->texture;
    mask.load_action = LoadAction::kClear;
    mask.store_action = StoreAction::kStore;
    clip_mask_scratch.SetColorAttachment(mask, 0u);
    clip_mask_scratch.SetStencilAttachment(*colour.GetStencilAttachment());
    clip_mask_scratch.SetDepthAttachment(*colour.GetDepthAttachment());
    // These standing target references borrow the existing D/S allocation;
    // physical bytes and creation events remain counted exactly once.
    for (size_t i = 0; i < coverage_count; ++i) {
      if (coverage_resources[i].texture ==
          colour.GetStencilAttachment()->texture) {
        coverage_resources[i].owned_references += 2u;
      }
    }
    clip_mask_resource = coverage_count;
    coverage_resources[coverage_count++] = *resource;
    interval.coverage.created_entries++;
    interval.coverage.created_real_bytes += resource->real;
    return clip_mask_scratch.IsValid();
  }

  bool CreateColourSeed(std::string* error) {
    size_t nominal = 0, real = 0;
    for (size_t i = 0; i < coverage_count; ++i) {
      nominal += coverage_resources[i].nominal;
      real += coverage_resources[i].real;
    }
    const AvioResourceAllocationScope kind(
        AvioRenderResourceKind::kCoverageRegion);
    auto resource = Allocate(
        Descriptor(config.colour_size, config.colour_format, false),
        "Avio GLES Prefix Scratch", config.coverage_max_bytes - nominal,
        config.coverage_max_bytes - real, error);
    if (!resource || coverage_count == coverage_resources.size())
      return false;
    colour_seed = resource->texture;
    coverage_resources[coverage_count++] = *resource;
    interval.coverage.created_entries++;
    interval.coverage.created_real_bytes += resource->real;
    return true;
  }

  bool CreateContinuousDestinationPrefix(std::string* error) {
    size_t nominal = 0, real = 0;
    for (size_t i = 0; i < coverage_count; ++i) {
      nominal += coverage_resources[i].nominal;
      real += coverage_resources[i].real;
    }
    const AvioResourceAllocationScope kind(
        AvioRenderResourceKind::kCoverageRegion);
    auto descriptor =
        Descriptor(config.colour_size, config.colour_format, true);
    descriptor.usage |= TextureUsage::kShaderRead;
    auto resource = Allocate(descriptor, "Avio Continuous Native Prefix",
                             config.coverage_max_bytes - nominal,
                             config.coverage_max_bytes - real, error);
    if (!resource || coverage_count == coverage_resources.size()) {
      return false;
    }
    continuous_destination_prefix = resource->texture;
    coverage_resources[coverage_count++] = *resource;
    interval.coverage.created_entries++;
    interval.coverage.created_real_bytes += resource->real;
    return true;
  }

  bool CreateWarmLayers(std::string* error) {
    const AvioResourceAllocationScope kind_scope(
        AvioRenderResourceKind::kLayerRegion);
    size_t nominal = 0u;
    size_t real = 0u;
    for (const auto& size : config.layer_sizes) {
      auto resource =
          Allocate(Descriptor(size, config.colour_format, false),
                   "Avio Layer Region", config.layer_max_bytes - nominal,
                   config.layer_max_bytes - real, error);
      if (!resource) {
        return false;
      }
      nominal += resource->nominal;
      real += resource->real;
      auto& layer = layers[layer_count++];
      layer.resource = std::move(*resource);
      ColorAttachment color;
      color.texture = layer.resource.texture;
      color.load_action = LoadAction::kClear;
      color.store_action = StoreAction::kStore;
      layer.target.SetColorAttachment(color, 0u);
      interval.layers.created_entries++;
      interval.layers.created_real_bytes += layer.resource.real;
    }
    warm_layers = layer_count;
    return true;
  }
};

AvioCoverageRegion::Lease::Lease(std::shared_ptr<State> state,
                                 Kind kind,
                                 uint64_t frame,
                                 AvioRegionPacking::Allocation allocation,
                                 bool overflow,
                                 bool clip_mask_scratch)
    : state_(state),
      kind_(kind),
      frame_(frame),
      token_(allocation.token),
      area_(ToRect(allocation.area)),
      content_(ToRect(allocation.content)),
      overflow_(overflow),
      clip_mask_scratch_(clip_mask_scratch) {}

AvioCoverageRegion::Lease::~Lease() {
  // Unlike explicit invalidation, final destruction proves that no cached
  // snapshot, recorder or native submission still owns this logical claim.
  // Never read or mutate the raster owner's plain state from this thread.
  if (const auto state = state_.lock()) {
    state->Return(kind_, token_, clip_mask_scratch_);
  }
}

bool AvioCoverageRegion::Lease::IsValid() const {
  const auto state = state_.lock();
  return !released_.load(std::memory_order_acquire) && state &&
         (clip_mask_scratch_ ? state->IsClipMaskLive(frame_, token_)
                             : state->IsLive(kind_, frame_, token_));
}

void AvioCoverageRegion::Lease::Release() {
  released_.store(true, std::memory_order_release);
}

RenderTarget AvioCoverageRegion::Lease::GetRenderTarget() const {
  const auto state = state_.lock();
  if (!IsValid() || !state) {
    return {};
  }
  auto target = clip_mask_scratch_ ? state->clip_mask_scratch
                                   : state->Target(kind_, token_);
  if (clip_mask_scratch_) {
    // Full clear is mandatory, including first use from Undefined layout.
    // Consumers obtain the explicit sampled content rectangle separately.
    return target;
  }
  if (!target.SetRenderArea(area_)) {
    return {};
  }
  if (kind_ == Kind::kLayer && !target.SetContentRect(content_)) {
    return {};
  }
  return target;
}

AvioCoverageRegion::AvioCoverageRegion(std::shared_ptr<State> state)
    : state_(std::move(state)) {}

std::shared_ptr<AvioCoverageRegion> AvioCoverageRegion::Create(
    std::shared_ptr<Allocator> allocator,
    const AvioCoverageRegionConfig& config,
    std::string* error) {
  if (error) {
    error->clear();
  }
  if (IsAvioRasterFrameActive()) {
    if (error) {
      *error = "Warm regions must be allocated before raster frame work.";
    }
    return nullptr;
  }
  if (!allocator || config.coverage_max_bytes == 0u ||
      config.layer_max_bytes == 0u || config.coverage_size.width < 8 ||
      config.coverage_size.height < 8 || config.colour_size.width < 8 ||
      config.colour_size.height < 8 || config.coverage_size.width % 8 != 0 ||
      config.coverage_size.height % 8 != 0 ||
      config.colour_size.width % 8 != 0 || config.colour_size.height % 8 != 0 ||
      (config.colour_depth_stencil_format != PixelFormat::kD24UnormS8Uint &&
       config.colour_depth_stencil_format != PixelFormat::kD32FloatS8UInt)) {
    if (error) {
      *error = "Invalid negotiated coverage region configuration.";
    }
    return nullptr;
  }
  auto state = std::make_shared<State>(std::move(allocator), config);
  if (config.cache_native_masks &&
      !state->CreateCoverageTarget(state->mask, config.coverage_size,
                                   PixelFormat::kR8UNormInt,
                                   PixelFormat::kS8UInt, false, error)) {
    return nullptr;
  }
  state->mask_resource_count = state->coverage_count;
  if (!state->CreateCoverageTarget(
          state->colour, config.colour_size, config.colour_format,
          config.colour_depth_stencil_format, true, error) ||
      (!config.cache_native_masks && !state->CreateColourSeed(error)) ||
      (config.cache_native_masks && !state->CreateClipMaskScratch(error)) ||
      (config.continuous_destination_prefix &&
       !state->CreateContinuousDestinationPrefix(error)) ||
      !state->CreateWarmLayers(error)) {
    return nullptr;
  }
  return std::shared_ptr<AvioCoverageRegion>(
      new AvioCoverageRegion(std::move(state)));
}

bool AvioCoverageRegion::BeginRasterFrame(uint64_t frame_epoch) {
  state_->DrainReturns();
  if (state_->frame_open || frame_epoch == 0u ||
      frame_epoch <= state_->last_frame) {
    return false;
  }
  state_->frame = state_->last_frame = frame_epoch;
  state_->frame_open = true;
  if (state_->mask_packing.LiveCount() == 0u) {
    state_->mask_packing.Reset();
  }
  return true;
}

bool AvioCoverageRegion::InitializeMaskOnce(
    const std::function<bool()>& initialize) {
  if (IsMaskInitialized()) {
    return true;
  }
  if (!initialize || IsAvioRasterFrameActive()) {
    return false;
  }
  std::call_once(state_->mask_initialize_once, [&] {
    state_->mask_initialized.store(initialize(), std::memory_order_release);
  });
  return IsMaskInitialized();
}

bool AvioCoverageRegion::InitializeColourOnce(
    const std::function<bool()>& initialize) {
  if (IsColourInitialized()) {
    return true;
  }
  if (!initialize || IsAvioRasterFrameActive()) {
    return false;
  }
  std::call_once(state_->colour_initialize_once, [&] {
    state_->colour_initialized.store(initialize(), std::memory_order_release);
  });
  return IsColourInitialized();
}

bool AvioCoverageRegion::IsMaskInitialized() const {
  return state_->mask_initialized.load(std::memory_order_acquire);
}

bool AvioCoverageRegion::IsColourInitialized() const {
  return state_->colour_initialized.load(std::memory_order_acquire);
}

void AvioCoverageRegion::EndRasterFrame() {
  state_->DrainReturns();
  if (!state_->frame_open) {
    return;
  }
  state_->UpdatePeak();
  state_->colour_packing.EndFrame();
  // Persistent static masks stay immutable until their final logical reader.
  // Command buffers retain GPU ownership separately from these CPU leases.
  if (state_->mask_packing.LiveCount() == 0u) {
    state_->mask_packing.Reset();
  }
  // Layer snapshots may be deferred or cached across frames. Their shared
  // leases pin distinct images until the last logical read has been encoded;
  // a frame boundary is neither a CPU-reader release nor a GPU fence.
  while (state_->layer_count > state_->warm_layers &&
         !state_->layers[state_->layer_count - 1u].resource.texture) {
    state_->layer_count--;
  }
  state_->frame_open = false;
}

AvioCoverageRegion::Acquisition AvioCoverageRegion::AcquireCoverage(
    Kind kind,
    ISize size) {
  state_->DrainReturns();
  if (!state_->frame_open) {
    return {Status::kFrameNotOpen, nullptr};
  }
  if (kind == Kind::kLayer) {
    return {Status::kInvalid, nullptr};
  }
  if ((kind == Kind::kMask && !IsMaskInitialized()) ||
      (kind == Kind::kColour && !IsColourInitialized())) {
    return {Status::kNotInitialized, nullptr};
  }
  auto reservation = state_->ReserveLease();
  if (!reservation)
    return {Status::kNeedsFlush, nullptr};
  auto& packing =
      kind == Kind::kMask ? state_->mask_packing : state_->colour_packing;
  auto result = packing.Allocate(size.width, size.height);
  switch (result.status) {
    case AvioRegionPacking::Status::kInvalid:
      return {Status::kInvalid, nullptr};
    case AvioRegionPacking::Status::kNeedsTiling:
      return {Status::kNeedsTiling, nullptr};
    case AvioRegionPacking::Status::kNeedsFlush:
      return {Status::kNeedsFlush, nullptr};
    case AvioRegionPacking::Status::kSuccess:
      break;
  }
  auto lease =
      reservation->Create(state_, kind, state_->frame, *result.allocation);
  auto& tokens =
      kind == Kind::kMask ? state_->mask_tokens : state_->colour_tokens;
  tokens[result.allocation->token.index] = result.allocation->token;
  state_->UpdatePeak();
  return {Status::kSuccess, std::move(lease)};
}

AvioCoverageRegion::Acquisition AvioCoverageRegion::AcquireClipMaskScratch(
    ISize size) {
  state_->DrainReturns();
  if (!state_->frame_open) {
    return {Status::kFrameNotOpen, nullptr};
  }
  if (!state_->config.cache_native_masks ||
      !state_->clip_mask_scratch.HasColorAttachment(0u) ||
      !IsColourInitialized()) {
    return {Status::kNotInitialized, nullptr};
  }
  if (size.width <= 0 || size.height <= 0 || state_->next_serial == 0u) {
    return {Status::kInvalid, nullptr};
  }
  if (!Fits(size, state_->config.colour_size)) {
    return {Status::kNeedsTiling, nullptr};
  }
  if (state_->clip_mask_live) {
    return {Status::kNeedsFlush, nullptr};
  }
  auto reservation = state_->ReserveLease();
  if (!reservation) {
    return {Status::kNeedsFlush, nullptr};
  }
  const auto serial = state_->next_serial++;
  const auto extent = state_->config.colour_size;
  const AvioRegionPacking::Allocation allocation = {
      {state_->frame, 0u, serial},
      {0, 0, extent.width, extent.height},
      {0, 0, size.width, size.height}};
  state_->clip_mask_live = true;
  state_->clip_mask_serial = serial;
  state_->clip_mask_frame = state_->frame;
  auto lease = reservation->Create(state_, Kind::kMask, state_->frame,
                                   allocation, false, true);
  state_->UpdatePeak();
  return {Status::kSuccess, std::move(lease)};
}

AvioCoverageRegion::Acquisition AvioCoverageRegion::AcquireLayer(
    ISize size,
    int32_t mip_count,
    bool exact_extent) {
  state_->DrainReturns();
  if (!state_->frame_open) {
    return {Status::kFrameNotOpen, nullptr};
  }
  if (mip_count < 1 ||
      !Fits(size, state_->allocator->GetMaxTextureSizeSupported()) ||
      state_->next_serial == 0u) {
    return {Status::kInvalid, nullptr};
  }
  auto reservation = state_->ReserveLease();
  if (!reservation)
    return {Status::kNeedsFlush, nullptr};
  size_t index = state_->warm_layers;
  int64_t best_area = std::numeric_limits<int64_t>::max();
  if (mip_count == 1) {
    for (size_t i = 0; i < state_->warm_layers; i++) {
      const auto extent = state_->config.layer_sizes[i];
      if (!state_->layers[i].live && Fits(size, extent) &&
          (!exact_extent || extent == size) &&
          extent.width * extent.height < best_area) {
        index = i;
        best_area = extent.width * extent.height;
      }
    }
  }
  bool overflow = index == state_->warm_layers;
  if (overflow) {
    index = state_->warm_layers;
    while (index < state_->layer_count &&
           state_->layers[index].resource.texture) {
      index++;
    }
    if (index == state_->layers.size()) {
      return {Status::kNeedsFlush, nullptr};
    }
    const AvioResourceAllocationScope kind_scope(
        AvioRenderResourceKind::kLayerRegion);
    auto resource = state_->Allocate(
        Descriptor(size, state_->config.colour_format, false, mip_count),
        "Avio Layer Region Overflow", std::numeric_limits<size_t>::max(),
        std::numeric_limits<size_t>::max(), nullptr);
    if (!resource) {
      return {Status::kAllocationFailed, nullptr};
    }
    state_->layer_count = std::max(state_->layer_count, index + 1u);
    auto& layer = state_->layers[index];
    layer.resource = std::move(*resource);
    ColorAttachment color;
    color.texture = layer.resource.texture;
    color.load_action = LoadAction::kClear;
    color.store_action = StoreAction::kStore;
    layer.target.SetColorAttachment(color, 0u);
    layer.overflow = true;
    state_->interval.layer_region_overflow++;
    state_->interval.layer_region_overflow_real_bytes += layer.resource.real;
    state_->interval.layers.created_entries++;
    state_->interval.layers.created_real_bytes += layer.resource.real;
  }
  auto& layer = state_->layers[index];
  layer.live = true;
  layer.serial = state_->next_serial++;
  layer.epoch = state_->frame;
  // A layer exclusively owns its physical image. Clearing the whole image
  // initializes first use and prevents stale filter taps outside its content.
  const auto physical_size = layer.target.GetRenderTargetSize();
  const AvioRegionPacking::Allocation allocation{
      {state_->frame, index, layer.serial},
      {0, 0, physical_size.width, physical_size.height},
      {0, 0, size.width, size.height}};
  auto lease = reservation->Create(state_, Kind::kLayer, state_->frame,
                                   allocation, overflow);
  state_->UpdatePeak();
  return {Status::kSuccess, std::move(lease)};
}

bool AvioCoverageRegion::FlushCoverageBatch(
    const std::function<bool()>& encode_and_composite) {
  state_->DrainReturns();
  if (!state_->frame_open || !encode_and_composite || !encode_and_composite()) {
    return false;
  }
  // Encoding can relinquish the final logical readers. Consume their exact
  // notifications before deciding whether this batch's packing can reset.
  state_->DrainReturns();
  if (state_->mask_packing.LiveCount() != 0u ||
      state_->colour_packing.LiveCount() != 0u) {
    return false;
  }
  state_->mask_packing.Reset();
  state_->colour_packing.Reset();
  return true;
}

void AvioCoverageRegion::RecordColourIslandComposite() {
  state_->interval.coverage_flushes++;
}

RenderTarget AvioCoverageRegion::GetColourIslandTarget() const {
  auto target = state_->colour;
  state_->UpdatePeak();
  return target;
}

RenderTarget AvioCoverageRegion::GetCoverageAtlasTarget() const {
  auto target = state_->mask;
  state_->UpdatePeak();
  return target;
}

RenderTarget AvioCoverageRegion::GetClipMaskScratchTarget() const {
  auto target = state_->clip_mask_scratch;
  state_->UpdatePeak();
  return target;
}

ISize AvioCoverageRegion::GetColourIslandSize() const {
  return state_->config.colour_size;
}

ISize AvioCoverageRegion::GetCoverageAtlasSize() const {
  return state_->config.coverage_size;
}

ISize AvioCoverageRegion::GetCoverageTileSize(Kind kind) const {
  if (kind == Kind::kLayer) {
    return {0, 0};
  }
  const auto rect = kind == Kind::kMask
                        ? state_->mask_packing.MaximumContent()
                        : state_->colour_packing.MaximumContent();
  return {rect.width, rect.height};
}

const AvioCoverageRegionConfig& AvioCoverageRegion::GetConfig() const {
  return state_->config;
}

AvioCoverageRegionUsage AvioCoverageRegion::ReportUsage(
    bool start_new_interval) {
  state_->DrainReturns();
  const auto result = state_->CurrentUsage();
  if (start_new_interval) {
    state_->interval = {};
  }
  return result;
}

bool AvioCoverageRegion::HasExactAllocatedBytes() const {
  return state_->config.require_exact_allocated_bytes;
}

bool AvioCoverageRegion::CachesNativeMasks() const {
  return state_->config.cache_native_masks;
}

std::shared_ptr<Texture> AvioCoverageRegion::GetColourSeedTexture() const {
  return state_->colour_seed;
}

std::shared_ptr<Texture> AvioCoverageRegion::GetContinuousDestinationPrefix()
    const {
  return state_->continuous_destination_prefix;
}

std::array<RenderTarget, 5> AvioCoverageRegion::GetWarmLayerTargets() const {
  std::array<RenderTarget, 5> targets;
  for (size_t i = 0; i < targets.size(); ++i) {
    targets[i] = state_->layers[i].target;
  }
  return targets;
}

AvioRenderResourceReport AvioCoverageRegion::GetDescriptorResourceReport(
    bool start_new_interval) const {
  state_->DrainReturns();
  AvioRenderResourceReport report;
  report.available = true;
  // Lifecycle counts belong to the observed region inventory as a whole,
  // including an overflow created/released before this report. Descriptor
  // rows below expose bytes/dimensions once, without pretending each physical
  // descriptor owns that aggregate interval's creation/destruction events.
  const auto usage = state_->CurrentUsage();
  auto add_counts = [&](AvioRenderResourceKind kind,
                        RenderResourceUsage counts) {
    counts.nominal_bytes = counts.real_bytes = 0;
    counts.peak_leased_nominal_bytes = counts.leased_entries = 0;
    counts.created_real_bytes = counts.orphans_released_real_bytes = 0;
    AvioRenderResourceEntry entry;
    entry.kind_id = static_cast<uint32_t>(kind);
    entry.usage = counts;
    entry.fields_supported = kAvioResourceFieldCounts;
    entry.unsupported_reason_id =
        kAvioResourceReasonPhysicalAllocationUnavailable;
    report.AddEntry(entry);
  };
  add_counts(AvioRenderResourceKind::kCoverageRegion, usage.coverage);
  add_counts(AvioRenderResourceKind::kLayerRegion, usage.layers);
  if (start_new_interval) {
    for (auto* interval :
         {&state_->interval.coverage, &state_->interval.layers}) {
      interval->created_entries = interval->orphans_released_entries = 0;
      interval->created_real_bytes = interval->orphans_released_real_bytes = 0;
    }
  }
  auto add = [&](AvioRenderResourceKind kind, const State::Resource& resource) {
    if (!resource.texture)
      return;
    const auto& desc = resource.texture->GetTextureDescriptor();
    AvioRenderResourceEntry entry;
    entry.kind_id = static_cast<uint32_t>(kind);
    entry.usage.entries = 1;
    entry.usage.nominal_bytes = resource.nominal;
    entry.fields_supported =
        kAvioResourceFieldDescriptorBytes | kAvioResourceFieldTextureDescriptor;
    if (state_->config.require_exact_allocated_bytes) {
      entry.fields_supported |= kAvioResourceFieldActualAllocatedBytes;
      entry.usage.real_bytes = resource.real;
    } else {
      entry.unsupported_reason_id =
          kAvioResourceReasonPhysicalAllocationUnavailable;
    }
    entry.descriptor_width = desc.size.width;
    entry.descriptor_height = desc.size.height;
    entry.descriptor_sample_count = static_cast<uint32_t>(desc.sample_count);
    entry.descriptor_format_id = static_cast<uint32_t>(desc.format);
    report.AddEntry(entry);
  };
  for (size_t i = 0; i < state_->coverage_count; ++i) {
    add(AvioRenderResourceKind::kCoverageRegion, state_->coverage_resources[i]);
  }
  for (size_t i = 0; i < state_->layer_count; ++i) {
    add(AvioRenderResourceKind::kLayerRegion, state_->layers[i].resource);
  }
  return report;
}

}  // namespace impeller
