// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/avio_coverage_region.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <utility>

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

struct AvioCoverageRegion::State {
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
  AvioCoverageRegionConfig config;
  RenderTarget mask;
  RenderTarget colour;
  AvioRegionPacking mask_packing;
  AvioRegionPacking colour_packing;
  std::array<Resource, 5> coverage_resources = {};
  size_t coverage_count = 0u;
  size_t mask_resource_count = 0u;
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
      AddUsage(coverage, coverage_resources[i],
               i < mask_resource_count ? mask_packing.LiveCount() > 0u
                                       : colour_packing.LiveCount() > 0u);
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
      AddUsage(coverage, coverage_resources[i],
               i < mask_resource_count ? mask_packing.LiveCount() > 0u
                                       : colour_packing.LiveCount() > 0u);
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
    const size_t real = texture->GetAllocatedByteSize();
    // This path is negotiated only on Vulkan, which knows its requirements.
    // Unknown bytes cannot prove that a permanent region fits its hard cap.
    if (real == 0u || real > remaining_real) {
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
                                 bool overflow)
    : state_(state),
      kind_(kind),
      frame_(frame),
      token_(allocation.token),
      area_(ToRect(allocation.area)),
      content_(ToRect(allocation.content)),
      overflow_(overflow) {}

AvioCoverageRegion::Lease::~Lease() {
  Release();
}

bool AvioCoverageRegion::Lease::IsValid() const {
  const auto state = state_.lock();
  return !released_ && state && state->IsLive(kind_, frame_, token_);
}

void AvioCoverageRegion::Lease::Release() {
  if (released_) {
    return;
  }
  if (const auto state = state_.lock()) {
    state->Release(kind_, frame_, token_);
  }
  released_ = true;
}

RenderTarget AvioCoverageRegion::Lease::GetRenderTarget() const {
  const auto state = state_.lock();
  if (released_ || !state || !state->IsLive(kind_, frame_, token_)) {
    return {};
  }
  auto target = state->Target(kind_, token_);
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
  if (!state->CreateCoverageTarget(state->mask, config.coverage_size,
                                   PixelFormat::kR8UNormInt,
                                   PixelFormat::kS8UInt, false, error)) {
    return nullptr;
  }
  state->mask_resource_count = state->coverage_count;
  if (!state->CreateCoverageTarget(
          state->colour, config.colour_size, config.colour_format,
          config.colour_depth_stencil_format, true, error) ||
      !state->CreateWarmLayers(error)) {
    return nullptr;
  }
  return std::shared_ptr<AvioCoverageRegion>(
      new AvioCoverageRegion(std::move(state)));
}

bool AvioCoverageRegion::BeginRasterFrame(uint64_t frame_epoch) {
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
  auto lease = std::shared_ptr<Lease>(
      new Lease(state_, kind, state_->frame, *result.allocation));
  state_->UpdatePeak();
  return {Status::kSuccess, std::move(lease)};
}

AvioCoverageRegion::Acquisition AvioCoverageRegion::AcquireLayer(
    ISize size,
    int32_t mip_count,
    bool exact_extent) {
  if (!state_->frame_open) {
    return {Status::kFrameNotOpen, nullptr};
  }
  if (mip_count < 1 ||
      !Fits(size, state_->allocator->GetMaxTextureSizeSupported()) ||
      state_->next_serial == 0u) {
    return {Status::kInvalid, nullptr};
  }
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
  auto lease = std::shared_ptr<Lease>(
      new Lease(state_, Kind::kLayer, state_->frame, allocation, overflow));
  state_->UpdatePeak();
  return {Status::kSuccess, std::move(lease)};
}

bool AvioCoverageRegion::FlushCoverageBatch(
    const std::function<bool()>& encode_and_composite) {
  if (!state_->frame_open || !encode_and_composite || !encode_and_composite()) {
    return false;
  }
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
  const auto result = state_->CurrentUsage();
  if (start_new_interval) {
    state_->interval = {};
  }
  return result;
}

}  // namespace impeller
