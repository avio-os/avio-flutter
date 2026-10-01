// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_RENDER_TARGET_CACHE_H_
#define FLUTTER_IMPELLER_ENTITY_RENDER_TARGET_CACHE_H_

#include <optional>
#include <string_view>
#include <vector>

#include "impeller/renderer/render_resource_usage.h"
#include "impeller/renderer/render_target.h"

namespace impeller {

/// @brief An implementation of the [RenderTargetAllocator] that caches all
///        allocated texture data for at least one frame.
///
///        Leases and aging are separate. A workload (one Canvas replay) opens
///        a lease scope; the targets it receives stay leased by it until the
///        scope closes, and nested scopes never share them. Aging happens once
///        per frame (`End`, called by the frame's owner after every view):
///        textures unused for [keep_alive_frame_count] frames are discarded.
///        Snapshot and per-view workloads therefore never age the cache.
class RenderTargetCache : public RenderTargetAllocator {
 public:
  explicit RenderTargetCache(std::shared_ptr<Allocator> allocator,
                             uint32_t keep_alive_frame_count = 4);

  ~RenderTargetCache() = default;

  // |RenderTargetAllocator|
  void Start() override;

  // |RenderTargetAllocator|
  void End() override;

  // |RenderTargetAllocator|
  uint64_t BeginScope() override;

  // |RenderTargetAllocator|
  void EndScope(uint64_t scope) override;

  // |RenderTargetAllocator|
  void DisableCache() override;

  // |RenderTargetAllocator|
  void EnableCache() override;

  RenderTarget CreateOffscreen(
      const Context& context,
      ISize size,
      int mip_count,
      std::string_view label = "Offscreen",
      RenderTarget::AttachmentConfig color_attachment_config =
          RenderTarget::kDefaultColorAttachmentConfig,
      std::optional<RenderTarget::AttachmentConfig> stencil_attachment_config =
          RenderTarget::kDefaultStencilAttachmentConfig,
      const std::shared_ptr<Texture>& existing_color_texture = nullptr,
      const std::shared_ptr<Texture>& existing_depth_stencil_texture = nullptr,
      std::optional<PixelFormat> target_pixel_format = std::nullopt) override;

  RenderTarget CreateOffscreenMSAA(
      const Context& context,
      ISize size,
      int mip_count,
      std::string_view label = "Offscreen MSAA",
      RenderTarget::AttachmentConfigMSAA color_attachment_config =
          RenderTarget::kDefaultColorAttachmentConfigMSAA,
      std::optional<RenderTarget::AttachmentConfig> stencil_attachment_config =
          RenderTarget::kDefaultStencilAttachmentConfig,
      const std::shared_ptr<Texture>& existing_color_msaa_texture = nullptr,
      const std::shared_ptr<Texture>& existing_color_resolve_texture = nullptr,
      const std::shared_ptr<Texture>& existing_depth_stencil_texture = nullptr,
      std::optional<PixelFormat> target_pixel_format = std::nullopt) override;

  /// Why a request allocated instead of reusing a cached target. Reported
  /// on the timeline as a `RenderTargetCacheMiss` instant.
  enum class MissReason {
    /// No cached target resembles the request.
    kNoEntry,
    /// An unleased target differs from the request only in its extent.
    kExtentMismatch,
    /// Every cached target with this exact key is leased.
    kAllLeased,
    /// A target with this exact key was recently dropped by aging (unused for
    /// the keep-alive number of frames).
    kAgedOut,
    /// The cache is disabled for this request.
    kDisabled,
  };

  static std::string_view MissReasonToString(MissReason reason);

  /// The reason of the latest miss, for tests.
  std::optional<MissReason> GetLastMissReasonForTesting() const {
    return last_miss_reason_;
  }

  // |RenderTargetAllocator|
  RenderResourceUsage ReportUsage(bool start_new_interval) override;

  // visible for testing.
  size_t CachedTextureCount() const;

 private:
  // Lease identities: unleased, leased outside any scope (released by the
  // next Start or End), or a scope id from BeginScope.
  static constexpr uint64_t kUnleased = 0u;
  static constexpr uint64_t kFrameScope = 1u;

  struct Scope {
    uint64_t id = kFrameScope;
    uint32_t disabled_count = 0u;
  };

  struct RenderTargetData {
    // Used since the previous aging epoch.
    bool used_this_frame;
    uint32_t keep_alive_frame_count;
    RenderTargetConfig config;
    RenderTarget render_target;
    // Accounted texel bytes and the allocations the backend made.
    size_t nominal_bytes = 0u;
    size_t real_bytes = 0u;
    // The report interval in which this entry was created.
    uint64_t created_interval = 0u;
    // The scope that leases this entry, or kUnleased.
    uint64_t lease_scope = kUnleased;
  };

  bool CacheEnabled() const;

  // The innermost open scope, or kFrameScope.
  uint64_t CurrentScope() const;

  // The disable count of the innermost open scope.
  uint32_t& CurrentDisabledCount();

  void ReleaseLeases(uint64_t scope);

  // Lease a cached entry.
  void LeaseEntry(RenderTargetData& data);

  // Record a created entry.
  void InsertEntry(const RenderTargetConfig& config,
                   const RenderTarget& render_target);

  // Account for an entry that leaves the cache.
  void AccountErased(const RenderTargetData& data);

  void SampleLeased();

  // Classify and trace a miss for `config`.
  void RecordMiss(const RenderTargetConfig& config, std::string_view label);

  // Remember a key that aging dropped.
  void RememberDropped(const RenderTargetConfig& config);

  // Trace the cache's size (entries, nominal and real bytes, as ReportUsage
  // accounts them) whenever it changes.
  void TraceCacheSize() const;

  std::vector<RenderTargetData> render_target_data_;
  uint32_t keep_alive_frame_count_;
  // The disable count outside any scope.
  uint32_t frame_disabled_count_ = 0;
  // Open lease scopes, innermost last.
  std::vector<Scope> scopes_;
  uint64_t next_scope_id_ = kFrameScope + 1u;
  // Report interval counters.
  uint64_t interval_ = 0u;
  size_t peak_leased_nominal_bytes_ = 0u;
  size_t created_entries_ = 0u;
  size_t created_erased_real_bytes_ = 0u;
  // Keys recently dropped by aging, oldest first.
  std::vector<RenderTargetConfig> recently_dropped_;
  std::optional<MissReason> last_miss_reason_;

  RenderTargetCache(const RenderTargetCache&) = delete;

  RenderTargetCache& operator=(const RenderTargetCache&) = delete;

 public:
  /// Visible for testing.
  std::vector<RenderTargetData>::const_iterator GetRenderTargetDataBegin()
      const {
    return render_target_data_.begin();
  }

  /// Visible for testing.
  std::vector<RenderTargetData>::const_iterator GetRenderTargetDataEnd() const {
    return render_target_data_.end();
  }
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_ENTITY_RENDER_TARGET_CACHE_H_
