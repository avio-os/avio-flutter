// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/render_target_cache.h"

#include <algorithm>
#include <vector>

#include "impeller/core/formats.h"
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

}  // namespace

RenderTargetCache::RenderTargetCache(std::shared_ptr<Allocator> allocator,
                                     uint32_t keep_alive_frame_count)
    : RenderTargetAllocator(std::move(allocator)),
      keep_alive_frame_count_(keep_alive_frame_count),
      clock_([] { return std::chrono::steady_clock::now(); }) {}

void RenderTargetCache::Start() {
  cache_disabled_count_ = 0;
  frame_open_ = true;
  for (auto& td : render_target_data_) {
    td.used_this_frame = false;
  }
}

void RenderTargetCache::End() {
  cache_disabled_count_ = 0;
  frame_open_ = false;
  std::vector<RenderTargetData> retain;

  for (RenderTargetData& td : render_target_data_) {
    if (td.used_this_frame) {
      retain.push_back(td);
    } else if (td.keep_alive_frame_count > 0) {
      td.keep_alive_frame_count--;
      retain.push_back(td);
    } else {
      AccountErased(td);
    }
  }
  render_target_data_.swap(retain);
}

void RenderTargetCache::LeaseEntry(RenderTargetData& data) {
  data.used_this_frame = true;
  data.keep_alive_frame_count = keep_alive_frame_count_;
  data.last_used = clock_();
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
      .last_used = clock_(),                              //
      .nominal_bytes = bytes.nominal,                     //
      .real_bytes = bytes.real,                           //
      .created_interval = interval_,                      //
  });
  created_entries_++;
  SampleLeased();
}

void RenderTargetCache::AccountErased(const RenderTargetData& data) {
  if (data.created_interval == interval_) {
    created_erased_real_bytes_ += data.real_bytes;
  }
}

void RenderTargetCache::SampleLeased() {
  if (!frame_open_) {
    return;
  }
  size_t leased = 0u;
  for (const RenderTargetData& td : render_target_data_) {
    if (td.used_this_frame) {
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
    if (frame_open_ && td.used_this_frame) {
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

ResourceCacheTrimResult RenderTargetCache::ReleaseIdle(
    std::chrono::nanoseconds unused_for) {
  const auto usage = [this]() {
    ResourceCacheUsage usage{.entries = render_target_data_.size()};
    for (const RenderTargetData& td : render_target_data_) {
      usage.bytes += td.nominal_bytes;
    }
    return usage;
  };
  ResourceCacheTrimResult result{.before = usage()};
  const auto now = clock_();
  std::vector<RenderTargetData> retain;
  for (RenderTargetData& td : render_target_data_) {
    if (frame_open_ && td.used_this_frame) {
      // Leased by the open workload.
      result.kept_in_use++;
      retain.push_back(td);
    } else if (now - td.last_used < unused_for) {
      result.kept_recent++;
      retain.push_back(td);
    } else {
      AccountErased(td);
    }
  }
  render_target_data_.swap(retain);
  result.after = usage();
  return result;
}

void RenderTargetCache::SetClockForTesting(Clock clock) {
  clock_ = std::move(clock);
}

void RenderTargetCache::DisableCache() {
  cache_disabled_count_++;
}

bool RenderTargetCache::CacheEnabled() const {
  return cache_disabled_count_ == 0;
}

void RenderTargetCache::EnableCache() {
  FML_DCHECK(cache_disabled_count_ > 0);
  if (cache_disabled_count_ == 0) {
    return;
  }
  cache_disabled_count_--;
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
  auto config = RenderTargetConfig{
      .size = size,
      .mip_count = static_cast<size_t>(mip_count),
      .has_msaa = false,
      .has_depth_stencil = stencil_attachment_config.has_value(),
  };

  if (CacheEnabled()) {
    for (RenderTargetData& render_target_data : render_target_data_) {
      const RenderTargetConfig other_config = render_target_data.config;
      if (!render_target_data.used_this_frame && other_config == config) {
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
  auto config = RenderTargetConfig{
      .size = size,
      .mip_count = static_cast<size_t>(mip_count),
      .has_msaa = true,
      .has_depth_stencil = stencil_attachment_config.has_value(),
  };
  if (CacheEnabled()) {
    for (RenderTargetData& render_target_data : render_target_data_) {
      const RenderTargetConfig other_config = render_target_data.config;
      if (!render_target_data.used_this_frame && other_config == config) {
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
