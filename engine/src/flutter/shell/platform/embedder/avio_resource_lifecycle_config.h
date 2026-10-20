// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_AVIO_RESOURCE_LIFECYCLE_CONFIG_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_AVIO_RESOURCE_LIFECYCLE_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "flutter/shell/platform/embedder/embedder.h"

namespace flutter {

// Shared by FlutterEngineInitialize and CPU contract tests. GLES uses the
// bounded coverage/layer regions from the antialiasing config, not Vulkan's
// legacy transient pool or persistent pipeline cache. Unsupported policy must
// be rejected before any context, directory duplicate or GPU object is created.
inline const char* ValidateAvioResourceLifecycleConfig(
    FlutterAvioExtensionFeatures features,
    const FlutterAvioResourceLifecycleConfig* config,
    const FlutterRendererConfig* renderer,
    bool enable_impeller,
    bool coverage_policy) {
  constexpr auto kCoverageRequirements =
      kFlutterAvioExtensionFeatureRootRenderTarget |
      kFlutterAvioExtensionFeatureResourceLifecycleConfig |
      kFlutterAvioExtensionFeatureRenderResourceReport;
  // Generic GTK GLES uses its own surface with bounded coverage and AA/report
  // negotiation, without the root selected-target lifecycle contract.
  const bool root_lifecycle =
      (renderer && renderer->type == kVulkan) ||
      (features & (kFlutterAvioExtensionFeatureOpenGLSelectedTarget |
                   kFlutterAvioExtensionFeatureResourceLifecycleConfig)) != 0;
  if (coverage_policy && root_lifecycle &&
      (features & kCoverageRequirements) != kCoverageRequirements) {
    return "Coverage requires root targets, resource limits and reports.";
  }
  if ((features & kFlutterAvioExtensionFeatureResourceLifecycleConfig) == 0) {
    return config == nullptr
               ? nullptr
               : "A resource lifecycle configuration was supplied without "
                 "negotiating its Avio extension feature.";
  }
  if (!config) {
    return "The negotiated Avio resource lifecycle configuration was missing.";
  }
  if (config->struct_size <
      offsetof(FlutterAvioResourceLifecycleConfig, pipeline_cache_max_bytes) +
          sizeof(config->pipeline_cache_max_bytes)) {
    return "The Avio resource lifecycle configuration was truncated.";
  }
  constexpr uint64_t kMaxSize = std::numeric_limits<size_t>::max();
  if (coverage_policy ? (config->transient_max_entries != 0u ||
                         config->transient_max_bytes != 0u)
                      : (config->transient_max_entries == 0u ||
                         config->transient_max_entries > kMaxSize ||
                         config->transient_max_bytes == 0u ||
                         config->transient_max_bytes > kMaxSize)) {
    return "The Avio transient resource limits were invalid.";
  }

  // Read the external C enum representation only after checking its bound.
  static_assert(sizeof(config->pipeline_cache_policy) == sizeof(uint32_t));
  uint32_t cache_policy;
  std::memcpy(&cache_policy, &config->pipeline_cache_policy,
              sizeof(cache_policy));
  switch (cache_policy) {
    case kFlutterAvioPipelineCacheDisabled:
      if (config->pipeline_cache_directory_fd != -1 ||
          config->pipeline_cache_max_bytes != 0u) {
        return "A disabled Avio pipeline cache must not carry a directory or "
               "byte budget.";
      }
      break;
    case kFlutterAvioPipelineCacheReadOnly:
    case kFlutterAvioPipelineCacheReadWrite:
      if (config->pipeline_cache_directory_fd < 0 ||
          config->pipeline_cache_max_bytes == 0u ||
          config->pipeline_cache_max_bytes > kMaxSize) {
        return "The Avio pipeline cache directory or byte budget was invalid.";
      }
      break;
    default:
      return "The Avio pipeline cache policy was unknown.";
  }

  if (!renderer || !enable_impeller ||
      (renderer->type != kVulkan && renderer->type != kOpenGL)) {
    return "The Avio resource lifecycle configuration requires Vulkan or "
           "OpenGL Impeller.";
  }
  if (renderer->type == kOpenGL &&
      (!coverage_policy ||
       (features & kFlutterAvioExtensionFeatureOpenGLSelectedTarget) == 0 ||
       cache_policy != kFlutterAvioPipelineCacheDisabled)) {
    return "OpenGL resource lifecycle requires negotiated selected targets, "
           "bounded coverage and a disabled Vulkan pipeline cache.";
  }
  return nullptr;
}

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_EMBEDDER_AVIO_RESOURCE_LIFECYCLE_CONFIG_H_
