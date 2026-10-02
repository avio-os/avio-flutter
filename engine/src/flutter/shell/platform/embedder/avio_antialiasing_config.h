// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_AVIO_ANTIALIASING_CONFIG_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_AVIO_ANTIALIASING_CONFIG_H_

#include <cstddef>
#include <cstring>
#include <limits>

#include "flutter/shell/platform/embedder/embedder.h"
#include "impeller/core/antialiasing_policy.h"

namespace flutter {

// Pure validation shared by production initialization and standalone contract
// tests. No caller-owned field beyond struct_size is read before its bound.
inline const char* ValidateAvioAntialiasingConfig(
    const FlutterAvioAntialiasingConfig* config,
    FlutterAvioExtensionFeatures features,
    bool coverage_implemented = impeller::kAvioCoveragePolicyImplemented) {
  const bool negotiated =
      (features & kFlutterAvioExtensionFeatureAntialiasingPolicy) != 0;
  if (!negotiated) {
    return config == nullptr ? nullptr
                             : "Antialiasing config was not negotiated.";
  }
  if (!config) {
    return "Negotiated antialiasing config was missing.";
  }
  if (config->struct_size <
      offsetof(FlutterAvioAntialiasingConfig, layer_region_max_bytes) +
          sizeof(config->layer_region_max_bytes)) {
    return "Antialiasing config was truncated.";
  }
  if ((config->continuous_requested_classes &
       ~impeller::kAvioContinuousKnownClasses) != 0) {
    return "Continuous coverage class identities are unknown.";
  }
  constexpr uint64_t kMaxSize = std::numeric_limits<size_t>::max();
  // This is an external C ABI enum. Read its representation without loading
  // an invalid C++ enum value before rejecting unknown inputs.
  static_assert(sizeof(config->policy) == sizeof(uint32_t));
  uint32_t policy;
  std::memcpy(&policy, &config->policy, sizeof(policy));
  switch (policy) {
    case kFlutterAvioAntialiasingPolicyMsaa4:
      if (config->continuous_requested_classes != 0 ||
          config->layer_sample_count != 4 ||
          config->coverage_sample_count != 4 ||
          config->coverage_region_max_bytes != 0 ||
          config->layer_region_max_bytes != 0) {
        return "The msaa4 policy requires legacy samples and no region "
               "budgets.";
      }
      break;
    case kFlutterAvioAntialiasingPolicyCoverage:
      if (!coverage_implemented) {
        return "Coverage antialiasing is not implemented in this build.";
      }
      if (config->layer_sample_count != 1 ||
          config->coverage_sample_count != 4 ||
          config->coverage_region_max_bytes == 0 ||
          config->coverage_region_max_bytes > kMaxSize ||
          config->layer_region_max_bytes == 0 ||
          config->layer_region_max_bytes > kMaxSize) {
        return "Coverage requires 1x layers, 4x masks and bounded regions.";
      }
      break;
    default:
      return "The antialiasing policy was unknown.";
  }
  return nullptr;
}

// Append-only query transport: each backend tail is independent, and an older
// caller's absent fields are left untouched. Masks describe this engine's
// compiled implementations, not the selected logical device's feature support.
inline void WriteAvioContinuousCapabilities(
    FlutterAvioExtensionCapabilities& capabilities,
    uint64_t vulkan,
    uint64_t gles,
    uint64_t metal) {
  const auto has = [&](size_t offset) {
    return capabilities.struct_size >= offset + sizeof(uint64_t);
  };
  if (has(offsetof(FlutterAvioExtensionCapabilities,
                   continuous_supported_classes))) {
    capabilities.continuous_supported_classes = vulkan | gles | metal;
  }
  if (has(offsetof(FlutterAvioExtensionCapabilities,
                   continuous_supported_classes_vulkan))) {
    capabilities.continuous_supported_classes_vulkan = vulkan;
  }
  if (has(offsetof(FlutterAvioExtensionCapabilities,
                   continuous_supported_classes_gles))) {
    capabilities.continuous_supported_classes_gles = gles;
  }
  if (has(offsetof(FlutterAvioExtensionCapabilities,
                   continuous_supported_classes_metal))) {
    capabilities.continuous_supported_classes_metal = metal;
  }
}

// Device/backend admission follows structural validation. A known identity
// does not imply an implementation on every backend.
inline const char* ValidateAvioAntialiasingBackend(
    const FlutterAvioAntialiasingConfig& config,
    impeller::AvioCoverageBackend backend) {
  return (config.continuous_requested_classes &
          ~impeller::AvioContinuousSupportedClasses(backend)) == 0
             ? nullptr
             : "Continuous coverage classes are unsupported by this backend.";
}

inline impeller::AvioAntialiasingConfig CopyAvioAntialiasingConfig(
    const FlutterAvioAntialiasingConfig& config) {
  return {
      .policy = static_cast<impeller::AvioAntialiasingPolicy>(config.policy),
      .layer_sample_count = config.layer_sample_count,
      .coverage_sample_count = config.coverage_sample_count,
      .continuous_requested_classes = config.continuous_requested_classes,
      .coverage_region_max_bytes =
          static_cast<size_t>(config.coverage_region_max_bytes),
      .layer_region_max_bytes =
          static_cast<size_t>(config.layer_region_max_bytes)};
}

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_EMBEDDER_AVIO_ANTIALIASING_CONFIG_H_
