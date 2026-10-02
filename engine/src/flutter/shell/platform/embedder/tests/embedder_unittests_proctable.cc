// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder.h"
#include "impeller/core/antialiasing_policy.h"

#include <cstddef>
#include <set>

#include "flutter/fml/build_config.h"
#include "flutter/testing/testing.h"

#ifdef _WIN32
// winbase.h defines GetCurrentTime as a macro.
#undef GetCurrentTime
#endif

// This test suite uses raw pointer arithmetic to iterate through a proc table.
// NOLINTBEGIN(clang-analyzer-security.ArrayBound)

namespace flutter {
namespace testing {

// Verifies that the proc table is fully populated.
TEST(EmbedderProcTable, AllPointersProvided) {
  FlutterEngineProcTable procs = {};
  procs.struct_size = sizeof(FlutterEngineProcTable);
  ASSERT_EQ(FlutterEngineGetProcAddresses(&procs), kSuccess);

  void (**proc)() = reinterpret_cast<void (**)()>(&procs.CreateAOTData);
  const uintptr_t end_address =
      reinterpret_cast<uintptr_t>(&procs) + procs.struct_size;
  while (reinterpret_cast<uintptr_t>(proc) < end_address) {
    EXPECT_NE(*proc, nullptr);
    ++proc;
  }
}

// Ensures that there are no duplicate pointers in the proc table, to catch
// copy/paste mistakes when adding a new entry to FlutterEngineGetProcAddresses.
TEST(EmbedderProcTable, NoDuplicatePointers) {
  FlutterEngineProcTable procs = {};
  procs.struct_size = sizeof(FlutterEngineProcTable);
  ASSERT_EQ(FlutterEngineGetProcAddresses(&procs), kSuccess);

  void (**proc)() = reinterpret_cast<void (**)()>(&procs.CreateAOTData);
  const uintptr_t end_address =
      reinterpret_cast<uintptr_t>(&procs) + procs.struct_size;
  std::set<void (*)()> seen_procs;
  while (reinterpret_cast<uintptr_t>(proc) < end_address) {
    auto result = seen_procs.insert(*proc);
    EXPECT_TRUE(result.second);
    ++proc;
  }
}

// Spot-checks that calling one of the function pointers works.
TEST(EmbedderProcTable, CallProc) {
  FlutterEngineProcTable procs = {};
  procs.struct_size = sizeof(FlutterEngineProcTable);
  ASSERT_EQ(FlutterEngineGetProcAddresses(&procs), kSuccess);

  EXPECT_NE(procs.GetCurrentTime(), 0ULL);
}

TEST(EmbedderProcTable, ReportsAvioSemanticCapabilities) {
  FlutterEngineProcTable procs = {};
  procs.struct_size = sizeof(FlutterEngineProcTable);
  ASSERT_EQ(FlutterEngineGetProcAddresses(&procs), kSuccess);
  ASSERT_NE(procs.GetAvioExtensionCapabilities, nullptr);

  FlutterAvioExtensionCapabilities capabilities = {};
  capabilities.struct_size = sizeof(capabilities);
  ASSERT_EQ(procs.GetAvioExtensionCapabilities(&capabilities), kSuccess);
  EXPECT_EQ(capabilities.minimum_version, FLUTTER_AVIO_EXTENSION_VERSION);
  EXPECT_EQ(capabilities.maximum_version, FLUTTER_AVIO_EXTENSION_VERSION);
  FlutterAvioExtensionFeatures expected_features =
      kFlutterAvioExtensionFeaturePerDisplayVsync |
      kFlutterAvioExtensionFeatureRootRenderTarget |
      kFlutterAvioExtensionFeatureExplicitRenderCompletion |
      kFlutterAvioExtensionFeatureExactVsyncCancellation |
      kFlutterAvioExtensionFeatureFrameOpportunityOutcomes |
      kFlutterAvioExtensionFeatureSelectedTargetDamage |
      kFlutterAvioExtensionFeatureViewVisibility |
      kFlutterAvioExtensionFeatureAtomicCompositorMaterials |
      kFlutterAvioExtensionFeatureTypedRenderTargetAcquisition |
      kFlutterAvioExtensionFeatureRenderDeadline |
      kFlutterAvioExtensionFeatureAtomicWindowPreviews |
      kFlutterAvioExtensionFeaturePreSubmitFailure |
      kFlutterAvioExtensionFeatureEmptyFrame |
      kFlutterAvioExtensionFeatureItemEffects |
      kFlutterAvioExtensionFeatureOutputGround |
      kFlutterAvioExtensionFeatureReadyContent;
#if FML_OS_LINUX && defined(SHELL_ENABLE_VULKAN) && IMPELLER_SUPPORTS_RENDERING
  expected_features |= kFlutterAvioExtensionFeatureResourceLifecycleConfig;
#endif
#if FML_OS_LINUX &&                                               \
    (defined(SHELL_ENABLE_VULKAN) || defined(SHELL_ENABLE_GL)) && \
    IMPELLER_SUPPORTS_RENDERING
  expected_features |= kFlutterAvioExtensionFeatureRenderResourceReport;
  if (impeller::kAvioCoveragePolicyImplemented) {
    expected_features |= kFlutterAvioExtensionFeatureAntialiasingPolicy;
  }
#endif
  EXPECT_EQ(capabilities.supported_features, expected_features);
  EXPECT_NE(procs.SetAvioViewVisibility, nullptr);
  EXPECT_NE(procs.RequestAvioRenderResourceReport, nullptr);
  uint64_t expected_vulkan = 0;
#if FML_OS_LINUX && defined(SHELL_ENABLE_VULKAN) && IMPELLER_SUPPORTS_RENDERING
  expected_vulkan = impeller::AvioContinuousSupportedClasses(
      impeller::AvioCoverageBackend::kVulkan);
#endif
  EXPECT_EQ(capabilities.continuous_supported_classes_vulkan, expected_vulkan);
  EXPECT_EQ(capabilities.continuous_supported_classes_gles, 0u);
  EXPECT_EQ(capabilities.continuous_supported_classes_metal, 0u);
  EXPECT_EQ(capabilities.continuous_supported_classes, expected_vulkan);
}

TEST(EmbedderProcTable, CapabilitiesAdvertiseVersion9) {
  FlutterEngineProcTable procs = {};
  procs.struct_size = sizeof(procs);
  ASSERT_EQ(FlutterEngineGetProcAddresses(&procs), kSuccess);
  ASSERT_NE(procs.GetAvioExtensionCapabilities, nullptr);
  FlutterAvioExtensionCapabilities capabilities = {};
  capabilities.struct_size = sizeof(capabilities);
  ASSERT_EQ(procs.GetAvioExtensionCapabilities(&capabilities), kSuccess);
  EXPECT_EQ(capabilities.minimum_version, 9u);
  EXPECT_EQ(capabilities.maximum_version, 9u);
  uint64_t expected_vulkan = 0;
#if FML_OS_LINUX && defined(SHELL_ENABLE_VULKAN) && IMPELLER_SUPPORTS_RENDERING
  expected_vulkan = impeller::AvioContinuousSupportedClasses(
      impeller::AvioCoverageBackend::kVulkan);
#endif
  EXPECT_EQ(capabilities.continuous_supported_classes_vulkan, expected_vulkan);
  EXPECT_EQ(capabilities.continuous_supported_classes_gles, 0u);
  EXPECT_EQ(capabilities.continuous_supported_classes_metal, 0u);
  EXPECT_EQ(capabilities.continuous_supported_classes, expected_vulkan);
}

TEST(EmbedderProcTable, AppendedCapabilityHonorsCallerStructSize) {
  FlutterEngineProcTable procs = {};
  procs.struct_size = sizeof(procs);
  ASSERT_EQ(FlutterEngineGetProcAddresses(&procs), kSuccess);
  ASSERT_NE(procs.GetAvioExtensionCapabilities, nullptr);
  FlutterAvioExtensionCapabilities capabilities = {};
  capabilities.struct_size =
      offsetof(FlutterAvioExtensionCapabilities, continuous_supported_classes);
  capabilities.continuous_supported_classes = 0xDEADBEEFu;
  ASSERT_EQ(procs.GetAvioExtensionCapabilities(&capabilities), kSuccess);
  EXPECT_EQ(capabilities.continuous_supported_classes, 0xDEADBEEFu);
}

TEST(EmbedderProcTable, ResourceReportRejectsMissingEngineWithoutCallback) {
  FlutterEngineProcTable procs = {};
  procs.struct_size = sizeof(procs);
  ASSERT_EQ(FlutterEngineGetProcAddresses(&procs), kSuccess);
  ASSERT_NE(procs.RequestAvioRenderResourceReport, nullptr);
  bool called = false;
  EXPECT_EQ(procs.RequestAvioRenderResourceReport(
                nullptr, false,
                [](const FlutterAvioRenderResourceReport*, void* opaque) {
                  *static_cast<bool*>(opaque) = true;
                },
                &called),
            kInvalidArguments);
  EXPECT_FALSE(called);
}

TEST(EmbedderProcTable, RejectsTruncatedAvioCapabilities) {
  FlutterEngineProcTable procs = {};
  procs.struct_size = sizeof(FlutterEngineProcTable);
  ASSERT_EQ(FlutterEngineGetProcAddresses(&procs), kSuccess);
  ASSERT_NE(procs.GetAvioExtensionCapabilities, nullptr);

  FlutterAvioExtensionCapabilities capabilities = {};
  capabilities.struct_size =
      offsetof(FlutterAvioExtensionCapabilities, supported_features);
  EXPECT_EQ(procs.GetAvioExtensionCapabilities(&capabilities),
            kInvalidArguments);
}

}  // namespace testing
}  // namespace flutter

// NOLINTEND(clang-analyzer-security.ArrayBound)
