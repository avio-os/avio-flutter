// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/render_target_cache.h"

#include <algorithm>
#include <string>
#include <vector>

#include "flutter/fml/trace_event.h"
#include "impeller/core/formats.h"
#include "impeller/renderer/context.h"
#include "impeller/renderer/render_target.h"

namespace impeller {

namespace {

struct RenderTargetBytes {
  size_t nominal = 0u;
  size_t real = 0u;
};

// Texel bytes and backend allocations of every distinct texture a render
// target attaches (depth and stencil usually share one texture).
RenderTargetBytes MeasureRenderTarget(const RenderTarget& target) {
  RenderTargetBytes bytes;
  std::vector<const Texture*> seen;
  const auto add = [&](const std::shared_ptr<Texture>& texture) {
    if (!texture ||
        std::find(seen.begin(), seen.end(), texture.get()) != seen.end()) {
      return;
    }
    seen.push_back(texture.get());
    const TextureDescriptor& desc = texture->GetTextureDescriptor();
    bytes.nominal += desc.GetByteSizeOfAllMipLevels() *
                     static_cast<size_t>(desc.sample_count);
    bytes.real += texture->GetAllocatedByteSize();
  };
  target.IterateAllAttachments([&](const Attachment& attachment) -> bool {
    add(attachment.texture);
    add(attachment.resolve_texture);
    return true;
  });
  return bytes;
}

// Bounds the memory of the aged-out classification.
constexpr size_t kRecentlyDroppedKeys = 32u;

}  // namespace

std::string_view RenderTargetCache::MissReasonToString(MissReason reason) {
  switch (reason) {
    case MissReason::kNoEntry:
      return "no_entry";
    case MissReason::kExtentMismatch:
      return "extent_mismatch";
    case MissReason::kAllLeased:
      return "all_leased";
    case MissReason::kAgedOut:
      return "aged_out";
    case MissReason::kDisabled:
      return "disabled";
  }
  FML_UNREACHABLE();
}

void RenderTargetCache::RecordMiss(const RenderTargetConfig& config,
                                   std::string_view label) {
  MissReason reason = MissReason::kNoEntry;
  if (!CacheEnabled()) {
    reason = MissReason::kDisabled;
  } else {
    bool same_key = false;
    bool other_extent = false;
    for (const RenderTargetData& td : render_target_data_) {
      if (td.config == config) {
        // A miss with an equal key means every such entry is leased.
        same_key = true;
      } else if (td.lease_scope == kUnleased &&
                 td.config.MatchesExceptSize(config)) {
        other_extent = true;
      }
    }
    if (same_key) {
      reason = MissReason::kAllLeased;
    } else if (std::find(recently_dropped_.begin(), recently_dropped_.end(),
                         config) != recently_dropped_.end()) {
      reason = MissReason::kAgedOut;
    } else if (other_extent) {
      reason = MissReason::kExtentMismatch;
    }
  }
  last_miss_reason_ = reason;
  const std::string target =
      "w=" + std::to_string(config.size.width) +
      " h=" + std::to_string(config.size.height) +
      " msaa=" + std::to_string(config.has_msaa) +
      " format=" + std::string(PixelFormatToString(config.color_format)) +
      " ds=" + std::to_string(config.has_depth_stencil) +
      " label=" + std::string(label);
  TRACE_EVENT_INSTANT2("impeller", "RenderTargetCacheMiss", "reason",
                       std::string(MissReasonToString(reason)).c_str(),
                       "target", target.c_str());
}

void RenderTargetCache::RememberDropped(const RenderTargetConfig& config) {
  if (recently_dropped_.size() == kRecentlyDroppedKeys) {
    recently_dropped_.erase(recently_dropped_.begin());
  }
  recently_dropped_.push_back(config);
}

void RenderTargetCache::TraceCacheSize() const {
  size_t nominal_bytes = 0u;
  size_t real_bytes = 0u;
  for (const RenderTargetData& td : render_target_data_) {
    nominal_bytes += td.nominal_bytes;
    real_bytes += td.real_bytes;
  }
  FML_TRACE_COUNTER("impeller", "RenderTargetCache",
                    reinterpret_cast<int64_t>(this), "entries",
                    static_cast<int64_t>(render_target_data_.size()), "bytes",
                    static_cast<int64_t>(nominal_bytes), "real_bytes",
                    static_cast<int64_t>(real_bytes));
}

RenderTargetCache::RenderTargetCache(std::shared_ptr<Allocator> allocator,
                                     uint32_t keep_alive_frame_count)
    : RenderTargetAllocator(std::move(allocator)),
      keep_alive_frame_count_(keep_alive_frame_count) {}

void RenderTargetCache::Start() {
  frame_disabled_count_ = 0;
  ReleaseLeases(kFrameScope);
}

void RenderTargetCache::End() {
  frame_disabled_count_ = 0;
  ReleaseLeases(kFrameScope);
  std::vector<RenderTargetData> retain;

  for (RenderTargetData& td : render_target_data_) {
    if (td.used_this_frame || td.lease_scope != kUnleased) {
      // Used in this epoch (or still leased by an open scope): keep it with
      // its full keep-alive, and start the next epoch unused.
      td.used_this_frame = false;
      retain.push_back(td);
    } else if (td.keep_alive_frame_count > 0) {
      td.keep_alive_frame_count--;
      retain.push_back(td);
    } else {
      AccountErased(td);
      RememberDropped(td.config);
    }
  }
  const bool changed = retain.size() != render_target_data_.size();
  render_target_data_.swap(retain);
  if (changed) {
    TraceCacheSize();
  }
}

uint64_t RenderTargetCache::BeginScope() {
  const uint64_t id = next_scope_id_++;
  scopes_.push_back(Scope{.id = id});
  return id;
}

void RenderTargetCache::EndScope(uint64_t scope) {
  const auto found =
      std::find_if(scopes_.begin(), scopes_.end(),
                   [scope](const Scope& open) { return open.id == scope; });
  FML_DCHECK(found != scopes_.end()) << "Closing a scope that is not open.";
  if (found == scopes_.end()) {
    return;
  }
  FML_DCHECK(found->disabled_count == 0u);
  scopes_.erase(found);
  ReleaseLeases(scope);
}

uint64_t RenderTargetCache::CurrentScope() const {
  return scopes_.empty() ? kFrameScope : scopes_.back().id;
}

uint32_t& RenderTargetCache::CurrentDisabledCount() {
  return scopes_.empty() ? frame_disabled_count_
                         : scopes_.back().disabled_count;
}

void RenderTargetCache::ReleaseLeases(uint64_t scope) {
  for (RenderTargetData& td : render_target_data_) {
    if (td.lease_scope == scope) {
      td.lease_scope = kUnleased;
    }
  }
}

void RenderTargetCache::LeaseEntry(RenderTargetData& data) {
  data.used_this_frame = true;
  data.lease_scope = CurrentScope();
  data.keep_alive_frame_count = keep_alive_frame_count_;
  SampleLeased();
}

void RenderTargetCache::InsertEntry(const RenderTargetConfig& config,
                                    const RenderTarget& render_target) {
  const RenderTargetBytes bytes = MeasureRenderTarget(render_target);
  render_target_data_.push_back(RenderTargetData{
      .used_this_frame = true,                            //
      .keep_alive_frame_count = keep_alive_frame_count_,  //
      .config = config,                                   //
      .render_target = render_target,                     //
      .nominal_bytes = bytes.nominal,                     //
      .real_bytes = bytes.real,                           //
      .created_interval = interval_,                      //
      .lease_scope = CurrentScope(),                      //
  });
  created_entries_++;
  SampleLeased();
  TraceCacheSize();
}

void RenderTargetCache::AccountErased(const RenderTargetData& data) {
  if (data.created_interval == interval_) {
    created_erased_real_bytes_ += data.real_bytes;
  }
}

void RenderTargetCache::SampleLeased() {
  size_t leased = 0u;
  for (const RenderTargetData& td : render_target_data_) {
    if (td.lease_scope != kUnleased) {
      leased += td.nominal_bytes;
    }
  }
  peak_leased_nominal_bytes_ = std::max(peak_leased_nominal_bytes_, leased);
}

RenderResourceUsage RenderTargetCache::ReportUsage(bool start_new_interval) {
  RenderResourceUsage report;
  report.entries = render_target_data_.size();
  size_t leased_nominal_bytes = 0u;
  size_t created_live_real_bytes = 0u;
  for (size_t index = 0; index < render_target_data_.size(); index++) {
    const RenderTargetData& td = render_target_data_[index];
    report.nominal_bytes += td.nominal_bytes;
    report.real_bytes += td.real_bytes;
    if (td.lease_scope != kUnleased) {
      report.leased_entries++;
      leased_nominal_bytes += td.nominal_bytes;
    }
    if (td.created_interval == interval_) {
      created_live_real_bytes += td.real_bytes;
    }
    const bool first_of_key = std::none_of(
        render_target_data_.begin(), render_target_data_.begin() + index,
        [&](const RenderTargetData& earlier) {
          return earlier.config == td.config;
        });
    if (first_of_key) {
      report.distinct_keys++;
    }
  }
  report.duplicate_entries = report.entries - report.distinct_keys;
  report.peak_leased_nominal_bytes =
      std::max(peak_leased_nominal_bytes_, leased_nominal_bytes);
  report.created_entries = created_entries_;
  report.created_real_bytes =
      created_erased_real_bytes_ + created_live_real_bytes;
  if (start_new_interval) {
    interval_++;
    peak_leased_nominal_bytes_ = 0u;
    created_entries_ = 0u;
    created_erased_real_bytes_ = 0u;
  }
  return report;
}

void RenderTargetCache::DisableCache() {
  CurrentDisabledCount()++;
}

bool RenderTargetCache::CacheEnabled() const {
  return (scopes_.empty() ? frame_disabled_count_
                          : scopes_.back().disabled_count) == 0;
}

void RenderTargetCache::EnableCache() {
  uint32_t& count = CurrentDisabledCount();
  FML_DCHECK(count > 0);
  if (count == 0) {
    return;
  }
  count--;
}

RenderTarget RenderTargetCache::CreateOffscreen(
    const Context& context,
    ISize size,
    int mip_count,
    std::string_view label,
    RenderTarget::AttachmentConfig color_attachment_config,
    std::optional<RenderTarget::AttachmentConfig> stencil_attachment_config,
    const std::shared_ptr<Texture>& existing_color_texture,
    const std::shared_ptr<Texture>& existing_depth_stencil_texture,
    std::optional<PixelFormat> target_pixel_format) {
  if (size.IsEmpty()) {
    return {};
  }

  FML_DCHECK(existing_color_texture == nullptr &&
             existing_depth_stencil_texture == nullptr);
  const auto& capabilities = context.GetCapabilities();
  auto config = RenderTargetConfig{
      .size = size,
      .mip_count = static_cast<size_t>(mip_count),
      .has_msaa = false,
      .has_depth_stencil = stencil_attachment_config.has_value(),
      .color_format =
          target_pixel_format.value_or(capabilities->GetDefaultColorFormat()),
      .color_storage = color_attachment_config.storage_mode,
  };
  if (stencil_attachment_config.has_value()) {
    config.depth_stencil_format = capabilities->GetDefaultDepthStencilFormat();
    config.depth_stencil_storage = stencil_attachment_config->storage_mode;
  }

  if (CacheEnabled()) {
    for (RenderTargetData& render_target_data : render_target_data_) {
      const RenderTargetConfig other_config = render_target_data.config;
      if (render_target_data.lease_scope == kUnleased &&
          other_config == config) {
        LeaseEntry(render_target_data);
        ColorAttachment color0 =
            render_target_data.render_target.GetColorAttachment(0);
        std::optional<DepthAttachment> depth =
            render_target_data.render_target.GetDepthAttachment();
        std::shared_ptr<Texture> depth_tex = depth ? depth->texture : nullptr;
        return RenderTargetAllocator::CreateOffscreen(
            context, size, mip_count, label, color_attachment_config,
            stencil_attachment_config, color0.texture, depth_tex,
            target_pixel_format);
      }
    }
  }
  RecordMiss(config, label);
  RenderTarget created_target = RenderTargetAllocator::CreateOffscreen(
      context, size, mip_count, label, color_attachment_config,
      stencil_attachment_config, nullptr, nullptr, target_pixel_format);
  if (!created_target.IsValid()) {
    return created_target;
  }
  if (CacheEnabled()) {
    InsertEntry(config, created_target);
  }
  return created_target;
}

RenderTarget RenderTargetCache::CreateOffscreenMSAA(
    const Context& context,
    ISize size,
    int mip_count,
    std::string_view label,
    RenderTarget::AttachmentConfigMSAA color_attachment_config,
    std::optional<RenderTarget::AttachmentConfig> stencil_attachment_config,
    const std::shared_ptr<Texture>& existing_color_msaa_texture,
    const std::shared_ptr<Texture>& existing_color_resolve_texture,
    const std::shared_ptr<Texture>& existing_depth_stencil_texture,
    std::optional<PixelFormat> target_pixel_format) {
  if (size.IsEmpty()) {
    return {};
  }

  FML_DCHECK(existing_color_msaa_texture == nullptr &&
             existing_color_resolve_texture == nullptr &&
             existing_depth_stencil_texture == nullptr);
  const auto& capabilities = context.GetCapabilities();
  auto config = RenderTargetConfig{
      .size = size,
      .mip_count = static_cast<size_t>(mip_count),
      .has_msaa = true,
      .has_depth_stencil = stencil_attachment_config.has_value(),
      .color_format =
          target_pixel_format.value_or(capabilities->GetDefaultColorFormat()),
      .color_storage = color_attachment_config.storage_mode,
      .resolve_storage = color_attachment_config.resolve_storage_mode,
  };
  if (stencil_attachment_config.has_value()) {
    config.depth_stencil_format = capabilities->GetDefaultDepthStencilFormat();
    config.depth_stencil_storage = stencil_attachment_config->storage_mode;
  }
  if (CacheEnabled()) {
    for (RenderTargetData& render_target_data : render_target_data_) {
      const RenderTargetConfig other_config = render_target_data.config;
      if (render_target_data.lease_scope == kUnleased &&
          other_config == config) {
        LeaseEntry(render_target_data);
        ColorAttachment color0 =
            render_target_data.render_target.GetColorAttachment(0);
        std::optional<DepthAttachment> depth =
            render_target_data.render_target.GetDepthAttachment();
        std::shared_ptr<Texture> depth_tex = depth ? depth->texture : nullptr;
        return RenderTargetAllocator::CreateOffscreenMSAA(
            context, size, mip_count, label, color_attachment_config,
            stencil_attachment_config, color0.texture, color0.resolve_texture,
            depth_tex, target_pixel_format);
      }
    }
  }
  RecordMiss(config, label);
  RenderTarget created_target = RenderTargetAllocator::CreateOffscreenMSAA(
      context, size, mip_count, label, color_attachment_config,
      stencil_attachment_config, nullptr, nullptr, nullptr,
      target_pixel_format);
  if (!created_target.IsValid()) {
    return created_target;
  }
  if (CacheEnabled()) {
    InsertEntry(config, created_target);
  }
  return created_target;
}

size_t RenderTargetCache::CachedTextureCount() const {
  return render_target_data_.size();
}

}  // namespace impeller
