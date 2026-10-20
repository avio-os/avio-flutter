// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder.h"
#include "impeller/core/antialiasing_policy.h"

#include <cstddef>
#include <iostream>
#include <set>
#include <sstream>

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
      kFlutterAvioExtensionFeatureReadyContent |
      kFlutterAvioExtensionFeatureHitRegions;
#if FML_OS_LINUX && defined(SHELL_ENABLE_GL) && IMPELLER_SUPPORTS_RENDERING
  expected_features |= kFlutterAvioExtensionFeatureOpenGLSelectedTarget;
#endif
#if FML_OS_LINUX &&                                               \
    (defined(SHELL_ENABLE_VULKAN) || defined(SHELL_ENABLE_GL)) && \
    IMPELLER_SUPPORTS_RENDERING
  expected_features |= kFlutterAvioExtensionFeatureResourceLifecycleConfig |
                       kFlutterAvioExtensionFeatureRenderResourceReport;
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

#if FML_OS_LINUX && defined(SHELL_ENABLE_GL) && IMPELLER_SUPPORTS_RENDERING
namespace {
// Exercise the real public initialization path, ending deliberately at the AOT
// source-conflict check immediately after resource-policy admission. No Dart VM,
// native context or fake rendered output is created by this argument test.
struct GlesInitializationArguments {
  const char* argv[2] = {"embedder-contract-test", "--enable-impeller"};
  uint8_t snapshot_sentinel = 0;
  bool native_context_requested = false;
  FlutterAvioExtensionRequest request = {};
  FlutterAvioResourceLifecycleConfig resources = {
      sizeof(resources), 0, 0, kFlutterAvioPipelineCacheDisabled, -1, 0};
  FlutterAvioAntialiasingConfig antialiasing = {
      sizeof(antialiasing), kFlutterAvioAntialiasingPolicyCoverage,
      1, 4, 0, 8u * 1024u * 1024u, 4u * 1024u * 1024u};
  FlutterRendererConfig renderer = {};
  FlutterProjectArgs args = {};

  explicit GlesInitializationArguments(FlutterAvioExtensionFeatures features) {
    request = {sizeof(request), FLUTTER_AVIO_EXTENSION_VERSION, features};
    renderer.type = kOpenGL;
    renderer.open_gl.struct_size = sizeof(renderer.open_gl);
    renderer.open_gl.make_current = [](void* user_data) {
      *static_cast<bool*>(user_data) = true;
      return false;
    };
    renderer.open_gl.clear_current = [](void*) { return true; };
    renderer.open_gl.present = [](void*) { return true; };
    renderer.open_gl.fbo_callback = [](void*) -> uint32_t { return 0; };
    args.struct_size = sizeof(args);
    args.assets_path = ".";
    args.command_line_argc = 2;
    args.command_line_argv = argv;
    args.vsync_for_display_callback = [](void*, intptr_t,
                                         FlutterEngineDisplayId) {};
    args.avio_extension_request = &request;
    args.avio_resource_lifecycle_config = &resources;
    args.avio_antialiasing_config = &antialiasing;
    // Neither pointer is dereferenced: their mutual presence is the explicit
    // post-admission invalid-argument sentinel, before snapshot loading.
    args.aot_data = reinterpret_cast<FlutterEngineAOTData>(&snapshot_sentinel);
    args.vm_snapshot_data = &snapshot_sentinel;
  }

  std::string InitializeAndCapture(FlutterEngineProcTable& procs) {
    FlutterEngine engine = nullptr;
    std::ostringstream errors;
    auto* original = std::cerr.rdbuf(errors.rdbuf());
    const auto result = procs.Initialize(FLUTTER_ENGINE_VERSION, &renderer,
                                         &args, &native_context_requested,
                                         &engine);
    std::cerr.rdbuf(original);
    EXPECT_EQ(result, kInvalidArguments);
    EXPECT_EQ(engine, nullptr);
    EXPECT_FALSE(native_context_requested);
    return errors.str();
  }
};
}  // namespace

TEST(EmbedderProcTable, GlesResourcePolicyPassesRealInitializationAdmission) {
  FlutterEngineProcTable procs = {};
  procs.struct_size = sizeof(procs);
  ASSERT_EQ(FlutterEngineGetProcAddresses(&procs), kSuccess);
  FlutterAvioExtensionCapabilities capabilities = {};
  capabilities.struct_size = sizeof(capabilities);
  ASSERT_EQ(procs.GetAvioExtensionCapabilities(&capabilities), kSuccess);
  GlesInitializationArguments input(capabilities.supported_features);
  EXPECT_NE(input.InitializeAndCapture(procs).find("Multiple AOT sources"),
            std::string::npos);
}

TEST(EmbedderProcTable, GenericGtkGlesCoveragePassesInitializationAdmission) {
  FlutterEngineProcTable procs = {};
  procs.struct_size = sizeof(procs);
  ASSERT_EQ(FlutterEngineGetProcAddresses(&procs), kSuccess);
  GlesInitializationArguments input(
      kFlutterAvioExtensionFeatureAntialiasingPolicy |
      kFlutterAvioExtensionFeatureRenderResourceReport);
  input.args.avio_resource_lifecycle_config = nullptr;
  EXPECT_NE(input.InitializeAndCapture(procs).find("Multiple AOT sources"),
            std::string::npos);
}

TEST(EmbedderProcTable, GlesResourcePolicyRejectsUnsupportedStateBeforeAot) {
  FlutterEngineProcTable procs = {};
  procs.struct_size = sizeof(procs);
  ASSERT_EQ(FlutterEngineGetProcAddresses(&procs), kSuccess);
  FlutterAvioExtensionCapabilities capabilities = {};
  capabilities.struct_size = sizeof(capabilities);
  ASSERT_EQ(procs.GetAvioExtensionCapabilities(&capabilities), kSuccess);
  for (auto policy : {kFlutterAvioPipelineCacheReadOnly,
                      kFlutterAvioPipelineCacheReadWrite}) {
    GlesInitializationArguments input(capabilities.supported_features);
    input.resources.pipeline_cache_policy = policy;
    input.resources.pipeline_cache_directory_fd = 0;
    input.resources.pipeline_cache_max_bytes = 64u * 1024u * 1024u;
    const auto error = input.InitializeAndCapture(procs);
    EXPECT_NE(error.find("disabled Vulkan pipeline cache"), std::string::npos);
    EXPECT_EQ(error.find("Multiple AOT sources"), std::string::npos);
  }
  GlesInitializationArguments input(capabilities.supported_features);
  input.resources.transient_max_entries = 1;
  input.resources.transient_max_bytes = 4096;
  EXPECT_NE(input.InitializeAndCapture(procs).find("transient resource limits"),
            std::string::npos);
  input.resources = {sizeof(input.resources), 0, 0,
                     kFlutterAvioPipelineCacheDisabled, -1, 0};
  input.request.required_features &=
      ~kFlutterAvioExtensionFeatureResourceLifecycleConfig;
  EXPECT_NE(input.InitializeAndCapture(procs).find("resource limits"),
            std::string::npos);
}
#endif

TEST(EmbedderProcTable, CapabilitiesAdvertiseVersion11) {
  FlutterEngineProcTable procs = {};
  procs.struct_size = sizeof(procs);
  ASSERT_EQ(FlutterEngineGetProcAddresses(&procs), kSuccess);
  ASSERT_NE(procs.GetAvioExtensionCapabilities, nullptr);
  FlutterAvioExtensionCapabilities capabilities = {};
  capabilities.struct_size = sizeof(capabilities);
  ASSERT_EQ(procs.GetAvioExtensionCapabilities(&capabilities), kSuccess);
  EXPECT_EQ(capabilities.minimum_version, 11u);
  EXPECT_EQ(capabilities.maximum_version, 11u);
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

TEST(EmbedderProcTable, SelectedTargetMembershipHonorsCallerStructSize) {
  FlutterEngineProcTable procs = {};
  procs.struct_size =
      offsetof(FlutterEngineProcTable, RegisterOpenGLSelectedTarget);
  ASSERT_EQ(FlutterEngineGetProcAddresses(&procs), kSuccess);
  EXPECT_EQ(procs.RegisterOpenGLSelectedTarget, nullptr);
  EXPECT_EQ(procs.UnregisterOpenGLSelectedTarget, nullptr);
  procs.struct_size = sizeof(procs);
  ASSERT_EQ(FlutterEngineGetProcAddresses(&procs), kSuccess);
  EXPECT_NE(procs.RegisterOpenGLSelectedTarget, nullptr);
  EXPECT_NE(procs.UnregisterOpenGLSelectedTarget, nullptr);
}

TEST(EmbedderProcTable, SelectedTargetRejectsMissingEngineWithoutCallback) {
  FlutterEngineProcTable procs = {};
  procs.struct_size = sizeof(procs);
  ASSERT_EQ(FlutterEngineGetProcAddresses(&procs), kSuccess);
  bool called = false;
  FlutterOpenGLSelectedTargetInfo info = {};
  info.struct_size = sizeof(info);
  info.texture.target = 0x0DE1;  // GL_TEXTURE_2D
  info.texture.name = 1;
  info.texture.format = 0x8058;  // GL_RGBA8
  info.texture.width = 8;
  info.texture.height = 8;
  info.user_data = &called;
  info.prepare_callback = [](void*) { return true; };
  info.callback = [](const FlutterOpenGLSelectedTargetResult* result) {
    *static_cast<bool*>(result->user_data) = true;
  };
  EXPECT_EQ(procs.RegisterOpenGLSelectedTarget(nullptr, &info),
            kInvalidArguments);
  EXPECT_EQ(
      procs.UnregisterOpenGLSelectedTarget(nullptr, 1, info.callback, &called),
      kInvalidArguments);
  EXPECT_FALSE(called);
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
