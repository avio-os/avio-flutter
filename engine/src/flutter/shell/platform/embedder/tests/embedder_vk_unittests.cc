// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define FML_USED_ON_EMBEDDER

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "embedder.h"
#include "embedder_engine.h"
#include "flutter/common/constants.h"
#include "flutter/fml/file.h"
#include "flutter/fml/synchronization/count_down_latch.h"
#include "flutter/shell/platform/embedder/tests/embedder_config_builder.h"
#include "flutter/shell/platform/embedder/tests/embedder_test.h"
#include "flutter/shell/platform/embedder/tests/embedder_test_backingstore_producer_vulkan.h"
#include "flutter/shell/platform/embedder/tests/embedder_test_context_vulkan.h"
#include "flutter/shell/platform/embedder/tests/embedder_unittests_util.h"
#include "flutter/testing/testing.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkImageInfo.h"

// CREATE_NATIVE_ENTRY is leaky by design
// NOLINTBEGIN(clang-analyzer-core.StackAddressEscape)

namespace flutter {
namespace testing {

using EmbedderTest = testing::EmbedderTest;

constexpr FlutterAvioExtensionFeatures kExactSelectedTargetFeatures =
    kFlutterAvioExtensionFeaturePerDisplayVsync |
    kFlutterAvioExtensionFeatureRootRenderTarget |
    kFlutterAvioExtensionFeatureExplicitRenderCompletion |
    kFlutterAvioExtensionFeatureExactVsyncCancellation |
    kFlutterAvioExtensionFeatureFrameOpportunityOutcomes |
    kFlutterAvioExtensionFeatureRenderDeadline |
    kFlutterAvioExtensionFeatureSelectedTargetDamage |
    kFlutterAvioExtensionFeaturePreSubmitFailure;

////////////////////////////////////////////////////////////////////////////////
// Notice: Other Vulkan unit tests exist in embedder_gl_unittests.cc.
//         See https://github.com/flutter/flutter/issues/134322
////////////////////////////////////////////////////////////////////////////////

namespace {

bool RasterImagesMatchOutsideRegion(const sk_sp<SkImage>& before,
                                    const sk_sp<SkImage>& after,
                                    const FlutterRect& region) {
  if (!before || !after || before->dimensions() != after->dimensions()) {
    return false;
  }
  const auto info =
      SkImageInfo::MakeN32Premul(before->width(), before->height());
  const size_t row_bytes = info.minRowBytes();
  std::vector<uint8_t> before_pixels(info.computeMinByteSize());
  std::vector<uint8_t> after_pixels(info.computeMinByteSize());
  if (!before->readPixels(nullptr, info, before_pixels.data(), row_bytes, 0,
                          0) ||
      !after->readPixels(nullptr, info, after_pixels.data(), row_bytes, 0, 0)) {
    return false;
  }
  for (int y = 0; y < before->height(); y++) {
    for (int x = 0; x < before->width(); x++) {
      if (x >= region.left && x < region.right && y >= region.top &&
          y < region.bottom) {
        continue;
      }
      const size_t offset = y * row_bytes + x * sizeof(uint32_t);
      if (std::memcmp(before_pixels.data() + offset,
                      after_pixels.data() + offset, sizeof(uint32_t)) != 0) {
        return false;
      }
    }
  }
  return true;
}

struct VulkanProcInfo {
  PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
  PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
  PFN_vkQueueSubmit queue_submit_proc_addr = nullptr;
  bool did_call_queue_submit = false;
};

struct SelectedTargetTestContext {
  struct Target {
    FlutterBackingStore backing_store = {};
    FlutterBackingStoreContentState content_state = {};
    void (*destruction_callback)(void*) = nullptr;
    void* destruction_user_data = nullptr;
    bool created = false;
  };

  explicit SelectedTargetTestContext(EmbedderTestCompositor& compositor,
                                     size_t target_count = 1u,
                                     bool external_handoff = true)
      : compositor(compositor),
        target_count(target_count),
        external_handoff(external_handoff) {
    FML_CHECK(target_count > 0u && target_count <= targets.size());
    for (size_t index = 0; index < targets.size(); index++) {
      targets[index].content_state = {
          .struct_size = sizeof(FlutterBackingStoreContentState),
          .target_identifier = 7u + index,
          .content_epoch = 1u,
          .preserved_contents = false,
          .existing_damage = nullptr,
      };
    }
  }

  ~SelectedTargetTestContext() {
    for (const Target& target : targets) {
      if (target.destruction_callback != nullptr) {
        target.destruction_callback(target.destruction_user_data);
      }
    }
  }

  bool Create(const FlutterBackingStoreConfig* config,
              FlutterBackingStore* backing_store_out) {
    requested_sizes.emplace_back(config->size.width, config->size.height);
    Target& target = targets[create_count % target_count];
    if (!target.created) {
      target.backing_store.struct_size = sizeof(FlutterBackingStore);
      if (!compositor.CreateBackingStore(config, &target.backing_store)) {
        return false;
      }
      target.destruction_callback =
          target.backing_store.vulkan.destruction_callback;
      target.destruction_user_data = target.backing_store.vulkan.user_data;
      target.backing_store.vulkan.destruction_callback = [](void*) {};
      target.created = true;
    }
    // Withholding ownership metadata disables bounded repaint in the engine,
    // but never skips the fixture's synchronization or Skia cache invalidation.
    if (!EmbedderTestBackingStoreProducerVulkan::PrepareForExternalRendering(
            &target.backing_store, external_handoff)) {
      return false;
    }
    target.backing_store.content_state = &target.content_state;
    *backing_store_out = target.backing_store;
    create_count++;
    return true;
  }

  bool Collect(const FlutterBackingStore* collected) {
    EXPECT_TRUE(
        std::any_of(targets.begin(), targets.end(), [&](const auto& target) {
          return target.created &&
                 target.backing_store.user_data == collected->user_data;
        }));
    collect_count++;
    return compositor.CollectBackingStore(collected);
  }

  bool Present(const FlutterPresentRenderTargetInfo& info) {
    present_count++;
    if (on_result && !on_result(info)) {
      return false;
    }
    if (info.status != kFlutterPresentRenderTargetStatusPresented) {
      return true;
    }

    if (!EmbedderTestBackingStoreProducerVulkan::CompleteExternalRendering(
            info.backing_store,
            info.backing_store_present_info->render_complete_sync_fd)) {
      return false;
    }
    FlutterLayer layer = {
        .struct_size = sizeof(FlutterLayer),
        .type = kFlutterLayerContentTypeBackingStore,
        .backing_store = info.backing_store,
        .offset = FlutterPoint{0.0, 0.0},
        .size = FlutterSize{0.0, 0.0},
        .backing_store_present_info =
            const_cast<FlutterBackingStorePresentInfo*>(
                info.backing_store_present_info),
        .presentation_time = 0,
        .shell_layer_role = kFlutterShellLayerRoleUnknown,
        .shell_visual_identifier = 0,
        .shell_visual_generation = 0,
        .shell_chrome_model_serial = 0,
    };
    const FlutterLayer* layers[] = {&layer};
    return compositor.Present(info.target_id, layers, 1);
  }

  void PreserveWithCatchUpDamage(size_t target = 0u) {
    FML_CHECK(target < target_count);
    targets[target].content_state.preserved_contents = true;
    targets[target].content_state.existing_damage = &catch_up_region;
  }

  void PreserveWithoutCatchUpDamage(size_t target = 0u) {
    FML_CHECK(target < target_count);
    targets[target].content_state.preserved_contents = true;
    targets[target].content_state.existing_damage = &empty_region;
  }

  void PreserveWithFullCatchUpDamage(size_t target = 0u) {
    FML_CHECK(target < target_count);
    targets[target].content_state.preserved_contents = true;
    targets[target].content_state.existing_damage = &full_region;
  }

  void Invalidate(size_t target = 0u) {
    FML_CHECK(target < target_count);
    targets[target].content_state.preserved_contents = false;
    targets[target].content_state.existing_damage = nullptr;
  }

  EmbedderTestCompositor& compositor;
  std::array<FlutterRect, 2> catch_up_rects = {
      FlutterRect{
          .left = 20.0,
          .top = 20.0,
          .right = 200.0,
          .bottom = 150.0,
      },
      FlutterRect{
          .left = 250.0,
          .top = 100.0,
          .right = 350.0,
          .bottom = 200.0,
      },
  };
  FlutterRegion catch_up_region = {
      .struct_size = sizeof(FlutterRegion),
      .rects_count = catch_up_rects.size(),
      .rects = catch_up_rects.data(),
  };
  FlutterRegion empty_region = {
      .struct_size = sizeof(FlutterRegion),
      .rects_count = 0,
      .rects = nullptr,
  };
  FlutterRect full_rect = {
      .left = 0.0,
      .top = 0.0,
      .right = 800.0,
      .bottom = 600.0,
  };
  FlutterRegion full_region = {
      .struct_size = sizeof(FlutterRegion),
      .rects_count = 1u,
      .rects = &full_rect,
  };
  std::array<Target, 3> targets;
  const size_t target_count;
  const bool external_handoff;
  size_t create_count = 0;
  size_t collect_count = 0;
  size_t present_count = 0;
  std::vector<DlISize> requested_sizes;
  std::function<bool(const FlutterPresentRenderTargetInfo&)> on_result;
};

FlutterRenderTargetAcquisition AcquireSelectedTarget(
    const FlutterRenderTargetAcquisitionInfo* info,
    FlutterBackingStore* backing_store_out) {
  return reinterpret_cast<SelectedTargetTestContext*>(info->user_data)
                 ->Create(info->config, backing_store_out)
             ? kFlutterRenderTargetAcquisitionGranted
             : kFlutterRenderTargetAcquisitionBackpressured;
}

static_assert(std::is_trivially_destructible_v<VulkanProcInfo>);

VulkanProcInfo g_vulkan_proc_info;

VkResult QueueSubmit(VkQueue queue,
                     uint32_t submitCount,
                     const VkSubmitInfo* pSubmits,
                     VkFence fence) {
  FML_DCHECK(g_vulkan_proc_info.queue_submit_proc_addr != nullptr);
  g_vulkan_proc_info.did_call_queue_submit = true;
  return g_vulkan_proc_info.queue_submit_proc_addr(queue, submitCount, pSubmits,
                                                   fence);
}

template <size_t N>
int StrcmpFixed(const char* str1, const char (&str2)[N]) {
  return strncmp(str1, str2, N - 1);
}

PFN_vkVoidFunction GetDeviceProcAddr(VkDevice device, const char* pName) {
  FML_DCHECK(g_vulkan_proc_info.get_device_proc_addr != nullptr);
  if (StrcmpFixed(pName, "vkQueueSubmit") == 0) {
    g_vulkan_proc_info.queue_submit_proc_addr =
        reinterpret_cast<PFN_vkQueueSubmit>(
            g_vulkan_proc_info.get_device_proc_addr(device, pName));
    return reinterpret_cast<PFN_vkVoidFunction>(QueueSubmit);
  }
  return g_vulkan_proc_info.get_device_proc_addr(device, pName);
}

PFN_vkVoidFunction GetInstanceProcAddr(VkInstance instance, const char* pName) {
  FML_DCHECK(g_vulkan_proc_info.get_instance_proc_addr != nullptr);
  if (StrcmpFixed(pName, "vkGetDeviceProcAddr") == 0) {
    g_vulkan_proc_info.get_device_proc_addr =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            g_vulkan_proc_info.get_instance_proc_addr(instance, pName));
    return reinterpret_cast<PFN_vkVoidFunction>(GetDeviceProcAddr);
  }
  return g_vulkan_proc_info.get_instance_proc_addr(instance, pName);
}

static_assert(
    std::is_same_v<decltype(&GetInstanceProcAddr), PFN_vkGetInstanceProcAddr>);
static_assert(
    std::is_same_v<decltype(&GetDeviceProcAddr), PFN_vkGetDeviceProcAddr>);
static_assert(std::is_same_v<decltype(&QueueSubmit), PFN_vkQueueSubmit>);
}  // namespace

TEST_F(EmbedderTest, CanGetVulkanEmbedderContext) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();
  EmbedderConfigBuilder builder(context);
}

TEST_F(EmbedderTest, CanSwapOutVulkanCalls) {
  fml::AutoResetWaitableEvent latch;

  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();
  context.AddIsolateCreateCallback([&latch]() { latch.Signal(); });
  context.SetVulkanInstanceProcAddressCallback(
      [](void* user_data, FlutterVulkanInstanceHandle instance,
         const char* name) -> void* {
        if (StrcmpFixed(name, "vkGetInstanceProcAddr") == 0) {
          g_vulkan_proc_info.get_instance_proc_addr =
              reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                  EmbedderTestContextVulkan::InstanceProcAddr(user_data,
                                                              instance, name));
          return reinterpret_cast<void*>(GetInstanceProcAddr);
        }
        return EmbedderTestContextVulkan::InstanceProcAddr(user_data, instance,
                                                           name);
      });

  EmbedderConfigBuilder builder(context);
  builder.SetSurface(DlISize(1024, 1024));
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Wait for the root isolate to launch.
  latch.Wait();
  engine.reset();
  EXPECT_TRUE(g_vulkan_proc_info.did_call_queue_submit);
}

////////////////////////////////////////////////////////////////////////////////
// Vulkan + Impeller compositor tests
////////////////////////////////////////////////////////////////////////////////

TEST_F(EmbedderTest, VulkanImpellerCompositorCanLaunchEngine) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetSurface(DlISize(800, 600));
  builder.SetCompositor();
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);
  builder.SetDartEntrypoint("render_impeller_test");

  // Verify the engine launches without hitting the "Unimplemented" error.
  fml::AutoResetWaitableEvent latch;
  context.AddIsolateCreateCallback([&latch]() { latch.Signal(); });

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  latch.Wait();
}

TEST_F(EmbedderTest, VulkanImpellerAcceptsExactResourceLifecycleConfig) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();
  fml::ScopedTemporaryDirectory cache_directory;

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetSurface(DlISize(64, 64));
  FlutterAvioExtensionRequest request = {
      .struct_size = sizeof(request),
      .version = FLUTTER_AVIO_EXTENSION_VERSION,
      .required_features = kFlutterAvioExtensionFeatureResourceLifecycleConfig,
  };
  FlutterAvioResourceLifecycleConfig resources = {
      .struct_size = sizeof(resources),
      .transient_max_entries = 3u,
      .transient_max_bytes = 16u * 1024u * 1024u,
      .pipeline_cache_policy = kFlutterAvioPipelineCacheReadOnly,
      .pipeline_cache_directory_fd = cache_directory.fd().get(),
      .pipeline_cache_max_bytes = 1024u * 1024u,
  };
  builder.GetProjectArgs().avio_extension_request = &request;
  builder.GetProjectArgs().avio_resource_lifecycle_config = &resources;

  auto engine = builder.InitializeEngine();
  ASSERT_TRUE(engine.is_valid());
  engine.reset();
  EXPECT_FALSE(
      fml::FileExists(cache_directory.fd(), "flutter.impeller.vkcache"));
}

TEST_F(EmbedderTest, VulkanImpellerCompositorPresentCallbackFires) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_impeller_test");
  builder.SetSurface(DlISize(800, 600));
  builder.SetCompositor();
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  fml::CountDownLatch latch(1);
  fml::CountDownLatch collect_latch(1);
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&collect_latch] { collect_latch.CountDown(); });
  context.GetCompositor().SetNextPresentCallback(
      [&](FlutterViewId view_id, const FlutterLayer** layers,
          size_t layers_count) {
        ASSERT_GT(layers_count, 0u);

        // The first layer should be a backing store rendered by Impeller.
        ASSERT_EQ(layers[0]->type, kFlutterLayerContentTypeBackingStore);
        ASSERT_EQ(layers[0]->backing_store->type,
                  kFlutterBackingStoreTypeVulkan);

        latch.CountDown();
      });

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);

  latch.Wait();
  engine.reset();
  collect_latch.Wait();
}

TEST_F(EmbedderTest, VulkanImpellerCompositorRendersScene) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_impeller_test");
  builder.SetSurface(DlISize(800, 600));
  builder.SetCompositor();
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  auto rendered_scene = context.GetNextSceneImage();
  fml::CountDownLatch collect_latch(1);
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&collect_latch] { collect_latch.CountDown(); });

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);

  // Verify that a scene was rendered (the future resolves with a non-null
  // image). Golden image comparison is deferred to a follow-up once the
  // reference images are captured with SwiftShader.
  auto scene_image = rendered_scene.get();
  ASSERT_TRUE(scene_image);
  engine.reset();
  collect_latch.Wait();
}

TEST_F(EmbedderTest, VulkanImpellerCompositorSkipsRootSurfaceAcquisition) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_impeller_test");
  builder.SetSurface(DlISize(800, 600));
  builder.SetCompositor();
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  // Verify that compositor present_view fires with valid layers, while
  // the root surface get_next_image/present_image callbacks are never called.
  fml::CountDownLatch latch(1);
  fml::CountDownLatch collect_latch(1);
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&collect_latch] { collect_latch.CountDown(); });
  context.GetCompositor().SetNextPresentCallback(
      [&](FlutterViewId view_id, const FlutterLayer** layers,
          size_t layers_count) {
        ASSERT_GT(layers_count, 0u);
        ASSERT_EQ(layers[0]->type, kFlutterLayerContentTypeBackingStore);
        ASSERT_EQ(layers[0]->backing_store->type,
                  kFlutterBackingStoreTypeVulkan);
        latch.CountDown();
      });

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);

  latch.Wait();

  // With compositor mode active, the root surface path should be skipped
  // entirely — no VkImage acquisition or presentation.
  EXPECT_EQ(context.GetNextImageCallCount(), 0u);
  EXPECT_EQ(context.GetSurfacePresentCount(), 0u);
  engine.reset();
  collect_latch.Wait();
}

TEST_F(EmbedderTest,
       SelectedTargetBudgetRefusalPreservesExactCollectibleLease) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();
  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_selected_target_ready");
  builder.AddDartEntrypointArgument("render_gradient_retained");
  fml::AutoResetWaitableEvent dart_ready;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&dart_ready](Dart_NativeArguments args) { dart_ready.Signal(); }));
  builder.SetSurface(DlISize(800, 600));
  builder.SetRootRenderTargetCompositor(
      true, kExactSelectedTargetFeatures |
                kFlutterAvioExtensionFeatureResourceLifecycleConfig);
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);
  // Force the real pool's byte-admission refusal before any target render.
  // The external image still exists; only its transient attachments are denied.
  FlutterAvioResourceLifecycleConfig resources = {
      .struct_size = sizeof(resources),
      .transient_max_entries = 6u,
      .transient_max_bytes = 64u,
      .pipeline_cache_policy = kFlutterAvioPipelineCacheDisabled,
      .pipeline_cache_directory_fd = -1,
  };
  builder.GetProjectArgs().avio_resource_lifecycle_config = &resources;
  SelectedTargetTestContext selected_target(context.GetCompositor());
  builder.GetCompositor().user_data = &selected_target;
  builder.GetCompositor().acquire_render_target_callback =
      AcquireSelectedTarget;
  builder.GetCompositor().collect_backing_store_callback =
      [](const FlutterBackingStore* store, void* user_data) {
        return reinterpret_cast<SelectedTargetTestContext*>(user_data)->Collect(
            store);
      };
  builder.GetCompositor().present_render_target_callback =
      [](const FlutterPresentRenderTargetInfo* info) {
        return reinterpret_cast<SelectedTargetTestContext*>(info->user_data)
            ->Present(*info);
      };
  fml::AutoResetWaitableEvent result_ready;
  fml::AutoResetWaitableEvent collected;
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&] { collected.Signal(); });
  selected_target.on_result = [&](const FlutterPresentRenderTargetInfo& info) {
    EXPECT_EQ(info.status,
              kFlutterPresentRenderTargetStatusAllocationFailedBeforeSubmit);
    EXPECT_NE(info.backing_store, nullptr);
    if (info.backing_store && info.backing_store->content_state) {
      EXPECT_EQ(info.backing_store->content_state->target_identifier, 7u);
    }
    result_ready.Signal();
    return true;
  };
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  ASSERT_FALSE(dart_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  for (int attempt = 0; attempt < 4; attempt++) {
    ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
              kSuccess);
    ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
    ASSERT_FALSE(collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  }
  engine.reset();
  EXPECT_EQ(selected_target.create_count, 4u);
  EXPECT_EQ(selected_target.collect_count, 4u);
  EXPECT_EQ(selected_target.present_count, 4u);
}

TEST_F(EmbedderTest,
       SelectedTargetDamageReacquiresAndRepaintsExactRetainedTarget) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_selected_target_ready");
  builder.AddDartEntrypointArgument("render_gradient_retained");
  fml::AutoResetWaitableEvent dart_ready;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&dart_ready](Dart_NativeArguments args) { dart_ready.Signal(); }));
  builder.SetSurface(DlISize(800, 600));
  builder.SetRootRenderTargetCompositor(
      /*avoid_backing_store_cache=*/false, kExactSelectedTargetFeatures);
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  SelectedTargetTestContext selected_target(context.GetCompositor(), 3u);
  builder.GetCompositor().user_data = &selected_target;
  builder.GetCompositor().acquire_render_target_callback =
      AcquireSelectedTarget;
  builder.GetCompositor().collect_backing_store_callback =
      [](const FlutterBackingStore* backing_store, void* user_data) {
        return reinterpret_cast<SelectedTargetTestContext*>(user_data)->Collect(
            backing_store);
      };
  builder.GetCompositor().present_render_target_callback =
      [](const FlutterPresentRenderTargetInfo* info) {
        return reinterpret_cast<SelectedTargetTestContext*>(info->user_data)
            ->Present(*info);
      };

  fml::AutoResetWaitableEvent result_ready;
  fml::AutoResetWaitableEvent target_collected;
  std::vector<FlutterPresentRenderTargetStatus> statuses;
  std::vector<uint64_t> target_identifiers;
  std::vector<int64_t> frame_damage_counts;
  std::vector<int64_t> buffer_damage_counts;
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&] { target_collected.Signal(); });
  selected_target.on_result = [&](const FlutterPresentRenderTargetInfo& info) {
    statuses.push_back(info.status);
    const auto* content_state =
        info.backing_store ? info.backing_store->content_state : nullptr;
    target_identifiers.push_back(
        content_state ? content_state->target_identifier : UINT64_MAX);
    const auto* present_info = info.backing_store_present_info;
    frame_damage_counts.push_back(
        present_info && present_info->frame_damage
            ? static_cast<int64_t>(present_info->frame_damage->rects_count)
            : -1);
    buffer_damage_counts.push_back(
        present_info && present_info->buffer_damage
            ? static_cast<int64_t>(present_info->buffer_damage->rects_count)
            : -1);
    result_ready.Signal();
    return true;
  };

  auto full_repaint_scene = context.GetNextSceneImage();
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Window metrics must follow Dart callback registration. Otherwise the
  // first frame can be delivered while the JIT entrypoint is still starting.
  ASSERT_FALSE(dart_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto full_repaint_image = full_repaint_scene.get();
  ASSERT_TRUE(full_repaint_image);
  selected_target.PreserveWithCatchUpDamage(0u);

  auto second_target_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto second_target_image = second_target_scene.get();
  ASSERT_TRUE(second_target_image);
  EXPECT_TRUE(RasterImagesAreSame(full_repaint_image, second_target_image,
                                  /*allowable_different_pixels=*/0));
  selected_target.PreserveWithoutCatchUpDamage(1u);

  auto third_target_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto third_target_image = third_target_scene.get();
  ASSERT_TRUE(third_target_image);
  EXPECT_TRUE(RasterImagesAreSame(full_repaint_image, third_target_image,
                                  /*allowable_different_pixels=*/0));

  auto partial_repaint_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto partial_repaint_image = partial_repaint_scene.get();
  ASSERT_TRUE(partial_repaint_image);
  EXPECT_TRUE(RasterImagesAreSame(full_repaint_image, partial_repaint_image,
                                  /*allowable_different_pixels=*/0));

  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));

  EXPECT_EQ(statuses, (std::vector<FlutterPresentRenderTargetStatus>{
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusNoVisualChange,
                      }));
  EXPECT_EQ(target_identifiers, (std::vector<uint64_t>{7u, 8u, 9u, 7u, 8u}));
  EXPECT_EQ(frame_damage_counts, (std::vector<int64_t>{1, 0, 0, 0, -1}));
  // The first three unknown targets require full repaint. The fourth frame
  // reuses preserved pixels and must actually honor the catch-up rectangle.
  EXPECT_EQ(buffer_damage_counts, (std::vector<int64_t>{-1, -1, -1, 1, -1}));
  EXPECT_EQ(selected_target.create_count, 5u);
  EXPECT_EQ(selected_target.collect_count, 5u);
  engine.reset();
}

TEST_F(EmbedderTest,
       SelectedTargetDamageClearsRemovedBlurWithFullRepaintParity) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_selected_target_ready");
  builder.AddDartEntrypointArgument("render_partial_repaint_clear_and_blur");
  fml::AutoResetWaitableEvent dart_ready;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&dart_ready](Dart_NativeArguments args) { dart_ready.Signal(); }));
  builder.SetSurface(DlISize(800, 600));
  builder.SetRootRenderTargetCompositor(
      /*avoid_backing_store_cache=*/false, kExactSelectedTargetFeatures);
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  SelectedTargetTestContext selected_target(context.GetCompositor());
  builder.GetCompositor().user_data = &selected_target;
  builder.GetCompositor().acquire_render_target_callback =
      AcquireSelectedTarget;
  builder.GetCompositor().collect_backing_store_callback =
      [](const FlutterBackingStore* backing_store, void* user_data) {
        return reinterpret_cast<SelectedTargetTestContext*>(user_data)->Collect(
            backing_store);
      };
  builder.GetCompositor().present_render_target_callback =
      [](const FlutterPresentRenderTargetInfo* info) {
        return reinterpret_cast<SelectedTargetTestContext*>(info->user_data)
            ->Present(*info);
      };

  fml::AutoResetWaitableEvent result_ready;
  fml::AutoResetWaitableEvent target_collected;
  std::vector<FlutterPresentRenderTargetStatus> statuses;
  std::vector<int64_t> frame_damage_counts;
  std::vector<int64_t> buffer_damage_counts;
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&] { target_collected.Signal(); });
  selected_target.on_result = [&](const FlutterPresentRenderTargetInfo& info) {
    statuses.push_back(info.status);
    const auto* present_info = info.backing_store_present_info;
    frame_damage_counts.push_back(
        present_info && present_info->frame_damage
            ? static_cast<int64_t>(present_info->frame_damage->rects_count)
            : -1);
    buffer_damage_counts.push_back(
        present_info && present_info->buffer_damage
            ? static_cast<int64_t>(present_info->buffer_damage->rects_count)
            : -1);
    result_ready.Signal();
    return true;
  };

  auto initial_scene = context.GetNextSceneImage();
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Window metrics must follow Dart callback registration. Otherwise the
  // first frame can be delivered while the JIT entrypoint is still starting.
  ASSERT_FALSE(dart_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto initial_image = initial_scene.get();
  ASSERT_TRUE(initial_image);

  selected_target.PreserveWithoutCatchUpDamage();
  auto partial_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto partial_image = partial_scene.get();
  ASSERT_TRUE(partial_image);

  selected_target.Invalidate();
  auto full_reference_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto full_reference_image = full_reference_scene.get();
  ASSERT_TRUE(full_reference_image);

  EXPECT_FALSE(RasterImagesAreSame(initial_image, partial_image,
                                   /*allowable_different_pixels=*/0));
  EXPECT_TRUE(RasterImagesAreSame(partial_image, full_reference_image,
                                  /*allowable_different_pixels=*/0));
  EXPECT_EQ(statuses, (std::vector<FlutterPresentRenderTargetStatus>{
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                      }));
  ASSERT_EQ(frame_damage_counts.size(), 3u);
  ASSERT_EQ(buffer_damage_counts.size(), 3u);
  // Removing the old blur must clear its former coverage while preserving
  // the rest of the image, with the same pixels as a forced full repaint.
  EXPECT_GT(frame_damage_counts[1], 0);
  EXPECT_EQ(buffer_damage_counts[1], 1);
  EXPECT_EQ(buffer_damage_counts[2], -1);
  EXPECT_EQ(selected_target.collect_count, 3u);
  engine.reset();
}

TEST_F(EmbedderTest,
       SelectedTargetDamageClearsFullyRemovedSceneBeforeNoVisualChange) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_selected_target_ready");
  builder.AddDartEntrypointArgument("render_then_clear_to_empty");
  fml::AutoResetWaitableEvent dart_ready;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&dart_ready](Dart_NativeArguments args) { dart_ready.Signal(); }));
  builder.SetSurface(DlISize(800, 600));
  builder.SetRootRenderTargetCompositor(
      /*avoid_backing_store_cache=*/false, kExactSelectedTargetFeatures);
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  SelectedTargetTestContext selected_target(context.GetCompositor());
  builder.GetCompositor().user_data = &selected_target;
  builder.GetCompositor().acquire_render_target_callback =
      AcquireSelectedTarget;
  builder.GetCompositor().collect_backing_store_callback =
      [](const FlutterBackingStore* backing_store, void* user_data) {
        return reinterpret_cast<SelectedTargetTestContext*>(user_data)->Collect(
            backing_store);
      };
  builder.GetCompositor().present_render_target_callback =
      [](const FlutterPresentRenderTargetInfo* info) {
        return reinterpret_cast<SelectedTargetTestContext*>(info->user_data)
            ->Present(*info);
      };

  fml::AutoResetWaitableEvent result_ready;
  fml::AutoResetWaitableEvent target_collected;
  std::vector<FlutterPresentRenderTargetStatus> statuses;
  std::vector<int64_t> frame_damage_counts;
  std::vector<int64_t> buffer_damage_counts;
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&] { target_collected.Signal(); });
  selected_target.on_result = [&](const FlutterPresentRenderTargetInfo& info) {
    statuses.push_back(info.status);
    const auto* present_info = info.backing_store_present_info;
    frame_damage_counts.push_back(
        present_info && present_info->frame_damage
            ? static_cast<int64_t>(present_info->frame_damage->rects_count)
            : -1);
    buffer_damage_counts.push_back(
        present_info && present_info->buffer_damage
            ? static_cast<int64_t>(present_info->buffer_damage->rects_count)
            : -1);
    result_ready.Signal();
    return true;
  };

  auto initial_scene = context.GetNextSceneImage();
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Window metrics must follow Dart callback registration. Otherwise the
  // first frame can be delivered while the JIT entrypoint is still starting.
  ASSERT_FALSE(dart_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto initial_image = initial_scene.get();
  ASSERT_TRUE(initial_image);

  selected_target.PreserveWithoutCatchUpDamage();
  auto cleared_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto cleared_image = cleared_scene.get();
  ASSERT_TRUE(cleared_image);

  selected_target.Invalidate();
  auto full_reference_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto full_reference_image = full_reference_scene.get();
  ASSERT_TRUE(full_reference_image);

  selected_target.PreserveWithoutCatchUpDamage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusNoVisualChange);

  EXPECT_FALSE(RasterImagesAreSame(initial_image, cleared_image,
                                   /*allowable_different_pixels=*/0));
  EXPECT_TRUE(RasterImagesAreSame(cleared_image, full_reference_image,
                                  /*allowable_different_pixels=*/0));
  EXPECT_EQ(statuses, (std::vector<FlutterPresentRenderTargetStatus>{
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusNoVisualChange,
                      }));
  ASSERT_EQ(frame_damage_counts.size(), 4u);
  ASSERT_EQ(buffer_damage_counts.size(), 4u);
  // Removing the scene clears its prior coverage in one bounded MSAA pass.
  EXPECT_GT(frame_damage_counts[1], 0);
  EXPECT_EQ(buffer_damage_counts[1], 1);
  EXPECT_EQ(buffer_damage_counts[2], -1);
  EXPECT_EQ(buffer_damage_counts[3], -1);
  EXPECT_EQ(selected_target.collect_count, 4u);
  engine.reset();
}

TEST_F(EmbedderTest,
       SelectedTargetDamageInitialEmptySceneDoesNotPublishUnknownTarget) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_selected_target_ready");
  builder.AddDartEntrypointArgument(
      "empty_scene_posts_zero_layers_to_compositor");
  fml::AutoResetWaitableEvent dart_ready;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&dart_ready](Dart_NativeArguments args) { dart_ready.Signal(); }));
  builder.SetSurface(DlISize(800, 600));
  builder.SetRootRenderTargetCompositor(
      /*avoid_backing_store_cache=*/false, kExactSelectedTargetFeatures);
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  SelectedTargetTestContext selected_target(context.GetCompositor());
  builder.GetCompositor().user_data = &selected_target;
  builder.GetCompositor().acquire_render_target_callback =
      AcquireSelectedTarget;
  builder.GetCompositor().collect_backing_store_callback =
      [](const FlutterBackingStore* backing_store, void* user_data) {
        return reinterpret_cast<SelectedTargetTestContext*>(user_data)->Collect(
            backing_store);
      };
  builder.GetCompositor().present_render_target_callback =
      [](const FlutterPresentRenderTargetInfo* info) {
        return reinterpret_cast<SelectedTargetTestContext*>(info->user_data)
            ->Present(*info);
      };

  fml::AutoResetWaitableEvent result_ready;
  fml::AutoResetWaitableEvent target_collected;
  FlutterPresentRenderTargetStatus status =
      kFlutterPresentRenderTargetStatusInternalInvariantViolation;
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&] { target_collected.Signal(); });
  selected_target.on_result = [&](const FlutterPresentRenderTargetInfo& info) {
    status = info.status;
    EXPECT_NE(info.backing_store, nullptr);
    EXPECT_EQ(info.backing_store_present_info, nullptr);
    result_ready.Signal();
    return true;
  };

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Window metrics must follow Dart callback registration. Otherwise the
  // first frame can be delivered while the JIT entrypoint is still starting.
  ASSERT_FALSE(dart_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));

  EXPECT_EQ(status, kFlutterPresentRenderTargetStatusNoVisualChange);
  EXPECT_EQ(selected_target.create_count, 1u);
  EXPECT_EQ(selected_target.present_count, 1u);
  EXPECT_EQ(selected_target.collect_count, 1u);
  engine.reset();
}

TEST_F(EmbedderTest,
       SelectedTargetDamageKeepsSparseFrameDamageForTranslucentGap) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_selected_target_ready");
  builder.AddDartEntrypointArgument(
      "render_disjoint_partial_repaint_with_translucent_gap");
  fml::AutoResetWaitableEvent dart_ready;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&dart_ready](Dart_NativeArguments args) { dart_ready.Signal(); }));
  builder.SetSurface(DlISize(800, 600));
  builder.SetRootRenderTargetCompositor(
      /*avoid_backing_store_cache=*/false, kExactSelectedTargetFeatures);
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  SelectedTargetTestContext selected_target(context.GetCompositor());
  builder.GetCompositor().user_data = &selected_target;
  builder.GetCompositor().acquire_render_target_callback =
      AcquireSelectedTarget;
  builder.GetCompositor().collect_backing_store_callback =
      [](const FlutterBackingStore* backing_store, void* user_data) {
        return reinterpret_cast<SelectedTargetTestContext*>(user_data)->Collect(
            backing_store);
      };
  builder.GetCompositor().present_render_target_callback =
      [](const FlutterPresentRenderTargetInfo* info) {
        return reinterpret_cast<SelectedTargetTestContext*>(info->user_data)
            ->Present(*info);
      };

  fml::AutoResetWaitableEvent result_ready;
  fml::AutoResetWaitableEvent target_collected;
  std::vector<FlutterPresentRenderTargetStatus> statuses;
  std::vector<int64_t> frame_damage_counts;
  std::vector<int64_t> buffer_damage_counts;
  std::vector<std::vector<FlutterRect>> frame_damage_rects;
  std::vector<std::vector<FlutterRect>> buffer_damage_rects;
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&] { target_collected.Signal(); });
  selected_target.on_result = [&](const FlutterPresentRenderTargetInfo& info) {
    statuses.push_back(info.status);
    const auto* present_info = info.backing_store_present_info;
    const auto copy_region = [](const FlutterRegion* region) {
      if (region == nullptr || region->rects_count == 0u) {
        return std::vector<FlutterRect>{};
      }
      return std::vector<FlutterRect>(region->rects,
                                      region->rects + region->rects_count);
    };
    const FlutterRegion* frame_damage =
        present_info ? present_info->frame_damage : nullptr;
    const FlutterRegion* buffer_damage =
        present_info ? present_info->buffer_damage : nullptr;
    frame_damage_counts.push_back(
        frame_damage ? static_cast<int64_t>(frame_damage->rects_count) : -1);
    buffer_damage_counts.push_back(
        buffer_damage ? static_cast<int64_t>(buffer_damage->rects_count) : -1);
    frame_damage_rects.push_back(copy_region(frame_damage));
    buffer_damage_rects.push_back(copy_region(buffer_damage));
    result_ready.Signal();
    return true;
  };

  auto initial_scene = context.GetNextSceneImage();
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Window metrics must follow Dart callback registration. Otherwise the
  // first frame can be delivered while the JIT entrypoint is still starting.
  ASSERT_FALSE(dart_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto initial_image = initial_scene.get();
  ASSERT_TRUE(initial_image);

  selected_target.PreserveWithoutCatchUpDamage();
  auto partial_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto partial_image = partial_scene.get();
  ASSERT_TRUE(partial_image);

  selected_target.Invalidate();
  auto full_reference_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto full_reference_image = full_reference_scene.get();
  ASSERT_TRUE(full_reference_image);

  EXPECT_FALSE(RasterImagesAreSame(initial_image, partial_image,
                                   /*allowable_different_pixels=*/0));
  EXPECT_TRUE(RasterImagesAreSame(partial_image, full_reference_image,
                                  /*allowable_different_pixels=*/0));
  EXPECT_EQ(statuses, (std::vector<FlutterPresentRenderTargetStatus>{
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                      }));
  ASSERT_EQ(frame_damage_counts.size(), 3u);
  ASSERT_EQ(buffer_damage_counts.size(), 3u);
  // Logical frame damage stays sparse across the disjoint regions -- it is
  // what accumulates catch-up damage for the other buffers, and coalescing it
  // to a bounding box would sweep the translucent gap between them into every
  // other target's history.
  EXPECT_GT(frame_damage_counts[1], 1);
  ASSERT_GT(frame_damage_rects[1].size(), 1u);

  // Raster damage is exactly the union bounds, while logical damage remains
  // sparse. The translucent gap is repainted once, not blended over old pixels.
  EXPECT_EQ(buffer_damage_counts, (std::vector<int64_t>{-1, 1, -1}));
  ASSERT_EQ(buffer_damage_rects[1].size(), 1u);
  const auto& raster_bounds = buffer_damage_rects[1][0];
  for (const auto& logical : frame_damage_rects[1]) {
    EXPECT_LE(raster_bounds.left, logical.left);
    EXPECT_LE(raster_bounds.top, logical.top);
    EXPECT_GE(raster_bounds.right, logical.right);
    EXPECT_GE(raster_bounds.bottom, logical.bottom);
  }
  EXPECT_LT((raster_bounds.right - raster_bounds.left) *
                (raster_bounds.bottom - raster_bounds.top),
            800.0 * 600.0);

  EXPECT_TRUE(RasterImagesMatchOutsideRegion(initial_image, partial_image,
                                             raster_bounds));
  RecordProperty("bounded_raster_rect",
                 std::to_string(raster_bounds.left) + "," +
                     std::to_string(raster_bounds.top) + "," +
                     std::to_string(raster_bounds.right) + "," +
                     std::to_string(raster_bounds.bottom));

  // The gap between the two damaged regions is translucent, so a frame that
  // failed to repaint it -- or repainted it over stale contents -- would not
  // match the forced full repaint above.
  EXPECT_EQ(selected_target.collect_count, 3u);
  engine.reset();
}

TEST_F(EmbedderTest, SelectedTargetDamageFullFallbackClearsPreservedTarget) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_selected_target_ready");
  builder.AddDartEntrypointArgument("render_partial_repaint_clear_and_blur");
  fml::AutoResetWaitableEvent dart_ready;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&dart_ready](Dart_NativeArguments args) { dart_ready.Signal(); }));
  builder.SetSurface(DlISize(800, 600));
  builder.SetRootRenderTargetCompositor(
      /*avoid_backing_store_cache=*/false, kExactSelectedTargetFeatures);
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  SelectedTargetTestContext selected_target(context.GetCompositor());
  builder.GetCompositor().user_data = &selected_target;
  builder.GetCompositor().acquire_render_target_callback =
      AcquireSelectedTarget;
  builder.GetCompositor().collect_backing_store_callback =
      [](const FlutterBackingStore* backing_store, void* user_data) {
        return reinterpret_cast<SelectedTargetTestContext*>(user_data)->Collect(
            backing_store);
      };
  builder.GetCompositor().present_render_target_callback =
      [](const FlutterPresentRenderTargetInfo* info) {
        return reinterpret_cast<SelectedTargetTestContext*>(info->user_data)
            ->Present(*info);
      };

  fml::AutoResetWaitableEvent result_ready;
  fml::AutoResetWaitableEvent target_collected;
  std::vector<FlutterPresentRenderTargetStatus> statuses;
  std::vector<int64_t> buffer_damage_counts;
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&] { target_collected.Signal(); });
  selected_target.on_result = [&](const FlutterPresentRenderTargetInfo& info) {
    statuses.push_back(info.status);
    const auto* present_info = info.backing_store_present_info;
    buffer_damage_counts.push_back(
        present_info && present_info->buffer_damage
            ? static_cast<int64_t>(present_info->buffer_damage->rects_count)
            : -1);
    result_ready.Signal();
    return true;
  };

  auto initial_scene = context.GetNextSceneImage();
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Window metrics must follow Dart callback registration. Otherwise the
  // first frame can be delivered while the JIT entrypoint is still starting.
  ASSERT_FALSE(dart_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(initial_scene.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  auto initial_image = initial_scene.get();
  ASSERT_TRUE(initial_image);

  selected_target.PreserveWithFullCatchUpDamage();
  auto fallback_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(fallback_scene.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  auto fallback_image = fallback_scene.get();
  ASSERT_TRUE(fallback_image);

  selected_target.Invalidate();
  auto full_reference_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(full_reference_scene.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  auto full_reference_image = full_reference_scene.get();
  ASSERT_TRUE(full_reference_image);

  EXPECT_FALSE(RasterImagesAreSame(initial_image, fallback_image,
                                   /*allowable_different_pixels=*/0));
  EXPECT_TRUE(RasterImagesAreSame(fallback_image, full_reference_image,
                                  /*allowable_different_pixels=*/0));
  EXPECT_EQ(statuses, (std::vector<FlutterPresentRenderTargetStatus>{
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                      }));
  EXPECT_EQ(buffer_damage_counts, (std::vector<int64_t>{-1, -1, -1}));
  EXPECT_EQ(selected_target.collect_count, 3u);
  engine.reset();
}

TEST_F(EmbedderTest, SelectedTargetDamageClearsRecycledSaveLayerTargets) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_selected_target_ready");
  builder.AddDartEntrypointArgument(
      "render_partial_repaint_through_recycled_save_layers");
  fml::AutoResetWaitableEvent dart_ready;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&dart_ready](Dart_NativeArguments args) { dart_ready.Signal(); }));
  builder.SetSurface(DlISize(800, 600));
  builder.SetRootRenderTargetCompositor(
      /*avoid_backing_store_cache=*/false, kExactSelectedTargetFeatures);
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  SelectedTargetTestContext selected_target(context.GetCompositor());
  builder.GetCompositor().user_data = &selected_target;
  builder.GetCompositor().acquire_render_target_callback =
      AcquireSelectedTarget;
  builder.GetCompositor().collect_backing_store_callback =
      [](const FlutterBackingStore* backing_store, void* user_data) {
        return reinterpret_cast<SelectedTargetTestContext*>(user_data)->Collect(
            backing_store);
      };
  builder.GetCompositor().present_render_target_callback =
      [](const FlutterPresentRenderTargetInfo* info) {
        return reinterpret_cast<SelectedTargetTestContext*>(info->user_data)
            ->Present(*info);
      };

  fml::AutoResetWaitableEvent result_ready;
  fml::AutoResetWaitableEvent target_collected;
  std::vector<FlutterPresentRenderTargetStatus> statuses;
  std::vector<int64_t> buffer_damage_counts;
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&] { target_collected.Signal(); });
  selected_target.on_result = [&](const FlutterPresentRenderTargetInfo& info) {
    statuses.push_back(info.status);
    const auto* present_info = info.backing_store_present_info;
    const FlutterRegion* buffer_damage =
        present_info ? present_info->buffer_damage : nullptr;
    buffer_damage_counts.push_back(
        buffer_damage ? static_cast<int64_t>(buffer_damage->rects_count) : -1);
    result_ready.Signal();
    return true;
  };

  auto initial_scene = context.GetNextSceneImage();
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Window metrics must follow Dart callback registration. Otherwise the
  // first frame can be delivered while the JIT entrypoint is still starting.
  ASSERT_FALSE(dart_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(initial_scene.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  auto initial_image = initial_scene.get();
  ASSERT_TRUE(initial_image);

  // The second frame retires the first frame's save layer offscreen into the
  // render target cache and asks for a same-sized one, which it only partly
  // paints.
  selected_target.PreserveWithoutCatchUpDamage();
  auto partial_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(partial_scene.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  auto partial_image = partial_scene.get();
  ASSERT_TRUE(partial_image);

  // Same scene, forced through the full-repaint path. Any pixel the partial
  // frame inherited from a recycled offscreen shows up as a difference here.
  selected_target.Invalidate();
  auto full_reference_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(full_reference_scene.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  auto full_reference_image = full_reference_scene.get();
  ASSERT_TRUE(full_reference_image);

  EXPECT_FALSE(RasterImagesAreSame(initial_image, partial_image,
                                   /*allowable_different_pixels=*/0));
  EXPECT_TRUE(RasterImagesAreSame(partial_image, full_reference_image,
                                  /*allowable_different_pixels=*/0));
  EXPECT_EQ(statuses, (std::vector<FlutterPresentRenderTargetStatus>{
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                      }));
  // Parent damage stays bounded even though the save-layer's private target
  // clears fully. Recycled private storage cannot retain its previous tenant.
  ASSERT_EQ(buffer_damage_counts.size(), 3u);
  EXPECT_EQ(buffer_damage_counts, (std::vector<int64_t>{-1, 1, -1}));
  engine.reset();
}

TEST_F(EmbedderTest, SelectedTargetDamageRefusedWhenRootPassNeedsReadback) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_selected_target_ready");
  builder.AddDartEntrypointArgument(
      "render_partial_repaint_with_root_backdrop_filter");
  fml::AutoResetWaitableEvent dart_ready;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&dart_ready](Dart_NativeArguments args) { dart_ready.Signal(); }));
  builder.SetSurface(DlISize(800, 600));
  builder.SetRootRenderTargetCompositor(
      /*avoid_backing_store_cache=*/false, kExactSelectedTargetFeatures);
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  SelectedTargetTestContext selected_target(context.GetCompositor());
  builder.GetCompositor().user_data = &selected_target;
  builder.GetCompositor().acquire_render_target_callback =
      AcquireSelectedTarget;
  builder.GetCompositor().collect_backing_store_callback =
      [](const FlutterBackingStore* backing_store, void* user_data) {
        return reinterpret_cast<SelectedTargetTestContext*>(user_data)->Collect(
            backing_store);
      };
  builder.GetCompositor().present_render_target_callback =
      [](const FlutterPresentRenderTargetInfo* info) {
        return reinterpret_cast<SelectedTargetTestContext*>(info->user_data)
            ->Present(*info);
      };

  fml::AutoResetWaitableEvent result_ready;
  fml::AutoResetWaitableEvent target_collected;
  std::vector<FlutterPresentRenderTargetStatus> statuses;
  std::vector<int64_t> buffer_damage_counts;
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&] { target_collected.Signal(); });
  selected_target.on_result = [&](const FlutterPresentRenderTargetInfo& info) {
    statuses.push_back(info.status);
    const auto* present_info = info.backing_store_present_info;
    const FlutterRegion* buffer_damage =
        present_info ? present_info->buffer_damage : nullptr;
    buffer_damage_counts.push_back(
        buffer_damage ? static_cast<int64_t>(buffer_damage->rects_count) : -1);
    result_ready.Signal();
    return true;
  };

  auto initial_scene = context.GetNextSceneImage();
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Window metrics must follow Dart callback registration. Otherwise the
  // first frame can be delivered while the JIT entrypoint is still starting.
  ASSERT_FALSE(dart_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(initial_scene.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  auto initial_image = initial_scene.get();
  ASSERT_TRUE(initial_image);

  // The target is preserved and the frame damage is sparse, so the engine
  // would ordinarily raster only the damage. It must not: the root backdrop
  // filter makes Impeller copy a whole offscreen over this target.
  selected_target.PreserveWithoutCatchUpDamage();
  auto readback_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(readback_scene.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  auto readback_image = readback_scene.get();
  ASSERT_TRUE(readback_image);

  // Full catch-up damage rather than an invalidated target: the target stays
  // preserved, so only the damage differs between the two frames.
  selected_target.PreserveWithFullCatchUpDamage();
  auto full_reference_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(full_reference_scene.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  auto full_reference_image = full_reference_scene.get();
  ASSERT_TRUE(full_reference_image);

  EXPECT_FALSE(RasterImagesAreSame(initial_image, readback_image,
                                   /*allowable_different_pixels=*/0));
  EXPECT_TRUE(RasterImagesAreSame(readback_image, full_reference_image,
                                  /*allowable_different_pixels=*/0));
  EXPECT_EQ(statuses, (std::vector<FlutterPresentRenderTargetStatus>{
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                      }));
  // No buffer damage on any of the three frames: the readback frame published
  // a full repaint rather than a rectangle it did not honor.
  EXPECT_EQ(buffer_damage_counts, (std::vector<int64_t>{-1, -1, -1}));
  engine.reset();
}

namespace {

// Count pixels the raster covered partially. Impeller produces these only by
// rastering multisampled and resolving, so their presence is the observable
// consequence of a multisampled root pass and their absence is the observable
// consequence of a single-sample one.
size_t CountPartiallyCoveredPixels(
    const sk_sp<SkImage>& image,
    std::optional<SkIRect> region = std::nullopt) {
  if (!image) {
    return 0u;
  }
  const SkImageInfo info = SkImageInfo::MakeN32Premul(
      image->width(), image->height(), SkColorSpace::MakeSRGB());
  const size_t row_bytes = info.minRowBytes();
  std::vector<uint8_t> pixels(info.computeByteSize(row_bytes));
  if (!image->readPixels(info, pixels.data(), row_bytes, 0, 0)) {
    return 0u;
  }

  SkIRect bounds = SkIRect::MakeWH(image->width(), image->height());
  if (region && !bounds.intersect(*region)) {
    return 0u;
  }
  size_t partial = 0u;
  for (int y = bounds.top(); y < bounds.bottom(); y++) {
    const uint32_t* row =
        reinterpret_cast<const uint32_t*>(pixels.data() + y * row_bytes);
    for (int x = bounds.left(); x < bounds.right(); x++) {
      const uint32_t alpha = SkColorGetA(row[x]);
      if (alpha != 0u && alpha != 255u) {
        partial++;
      }
    }
  }
  return partial;
}

}  // namespace

// The defect this pins: the root pass used to drop to a single sample for any
// target whose contents the embedder had preserved, which is every target in a
// steady-state compositor session. Nothing downstream reported it -- the
// frames were correct, just unantialiased -- so the only way to catch a
// regression is to look at the pixels along an edge only multisampling can
// cover partially.
void CheckPreservedTargetMultisampling(EmbedderTestContextVulkan& context,
                                       bool external_handoff) {
  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_selected_target_ready");
  builder.AddDartEntrypointArgument("render_clipped_diagonal_edge");
  fml::AutoResetWaitableEvent dart_ready;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&dart_ready](Dart_NativeArguments args) { dart_ready.Signal(); }));
  builder.SetSurface(DlISize(800, 600));
  builder.SetRootRenderTargetCompositor(
      /*avoid_backing_store_cache=*/false, kExactSelectedTargetFeatures);
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  SelectedTargetTestContext selected_target(context.GetCompositor(), 1u,
                                            external_handoff);
  builder.GetCompositor().user_data = &selected_target;
  builder.GetCompositor().acquire_render_target_callback =
      AcquireSelectedTarget;
  builder.GetCompositor().collect_backing_store_callback =
      [](const FlutterBackingStore* backing_store, void* user_data) {
        return reinterpret_cast<SelectedTargetTestContext*>(user_data)->Collect(
            backing_store);
      };
  builder.GetCompositor().present_render_target_callback =
      [](const FlutterPresentRenderTargetInfo* info) {
        return reinterpret_cast<SelectedTargetTestContext*>(info->user_data)
            ->Present(*info);
      };

  fml::AutoResetWaitableEvent result_ready;
  fml::AutoResetWaitableEvent target_collected;
  std::vector<FlutterPresentRenderTargetStatus> statuses;
  std::vector<int64_t> buffer_damage_counts;
  std::vector<FlutterRect> honored_rects;
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&] { target_collected.Signal(); });
  selected_target.on_result = [&](const FlutterPresentRenderTargetInfo& info) {
    statuses.push_back(info.status);
    const auto* damage = info.backing_store_present_info
                             ? info.backing_store_present_info->buffer_damage
                             : nullptr;
    buffer_damage_counts.push_back(
        damage ? static_cast<int64_t>(damage->rects_count) : -1);
    if (damage && damage->rects_count == 1u) {
      honored_rects.push_back(damage->rects[0]);
    }
    result_ready.Signal();
    return true;
  };

  auto unpreserved_scene = context.GetNextSceneImage();
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Window metrics must follow Dart callback registration. Otherwise the
  // first frame can be delivered while the JIT entrypoint is still starting.
  ASSERT_FALSE(dart_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(unpreserved_scene.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  auto unpreserved_image = unpreserved_scene.get();
  ASSERT_TRUE(unpreserved_image);

  // Repaint a bounded region through the diagonal AA edge, leaving pixels
  // elsewhere intact. Legacy targets without the layout handoff stay full.
  selected_target.catch_up_rects[0] = FlutterRect{180, 140, 600, 430};
  selected_target.catch_up_region.rects_count = 1;
  selected_target.PreserveWithCatchUpDamage();
  auto preserved_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(preserved_scene.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  auto preserved_image = preserved_scene.get();
  ASSERT_TRUE(preserved_image);

  // The diagonal runs 580 pixels across and 430 down, so a multisampled edge
  // covers hundreds of pixels partially. A single-sample one covers none: the
  // threshold is far below what multisampling produces and far above the zero
  // a hard edge produces, so it does not need to track edge length exactly.
  const size_t unpreserved_partial =
      CountPartiallyCoveredPixels(unpreserved_image);
  const size_t preserved_partial = CountPartiallyCoveredPixels(preserved_image);
  EXPECT_GT(unpreserved_partial, 100u);
  EXPECT_GT(preserved_partial, 100u);
  EXPECT_GT(CountPartiallyCoveredPixels(preserved_image,
                                        SkIRect::MakeLTRB(180, 140, 600, 430)),
            100u);
  EXPECT_EQ(buffer_damage_counts,
            (std::vector<int64_t>{-1, external_handoff ? 1 : -1}));
  EXPECT_TRUE(RasterImagesMatchOutsideRegion(
      unpreserved_image, preserved_image, selected_target.catch_up_rects[0]));
  if (external_handoff) {
    ASSERT_EQ(honored_rects.size(), 1u);
    const auto& bounds = honored_rects[0];
    EXPECT_EQ(bounds.left, 180);
    EXPECT_EQ(bounds.top, 140);
    EXPECT_EQ(bounds.right, 600);
    EXPECT_EQ(bounds.bottom, 430);
    EXPECT_LT((bounds.right - bounds.left) * (bounds.bottom - bounds.top),
              800.0 * 600.0);
    ::testing::Test::RecordProperty(
        "bounded_raster_rect",
        std::to_string(bounds.left) + "," + std::to_string(bounds.top) + "," +
            std::to_string(bounds.right) + "," + std::to_string(bounds.bottom));
  } else {
    EXPECT_TRUE(honored_rects.empty());
  }

  // Same scene, same coverage: preserving a target changes nothing about how
  // its frame is rastered.
  EXPECT_TRUE(RasterImagesAreSame(unpreserved_image, preserved_image,
                                  /*allowable_different_pixels=*/0));
  engine.reset();
}

TEST_F(EmbedderTest, SelectedTargetDamageKeepsPreservedTargetMultisampled) {
  CheckPreservedTargetMultisampling(
      GetEmbedderContext<EmbedderTestContextVulkan>(), true);
}

TEST_F(EmbedderTest, SelectedTargetWithoutLayoutHandoffKeepsFullMSAARepaint) {
  CheckPreservedTargetMultisampling(
      GetEmbedderContext<EmbedderTestContextVulkan>(), false);
}

////////////////////////////////////////////////////////////////////////////////
// Idle render-resource release (Avio extension v7)
////////////////////////////////////////////////////////////////////////////////

namespace {

constexpr FlutterAvioExtensionFeatures kIdleReleaseFeatures =
    kExactSelectedTargetFeatures |
    kFlutterAvioExtensionFeatureResourceLifecycleConfig |
    kFlutterAvioExtensionFeatureIdleResourceRelease;

// Counts the vkDestroyImage calls the engine makes through the instance proc
// address callback the embedder supplies (installed with
// InstallImageDestroyCounter before the engine is configured).
struct ImageDestroyCounter {
  PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
  PFN_vkDestroyImage destroy_image = nullptr;
  std::atomic<size_t> destroyed = 0u;
};

ImageDestroyCounter g_image_destroy_counter;

void CountingDestroyImage(VkDevice device,
                          VkImage image,
                          const VkAllocationCallbacks* allocator) {
  g_image_destroy_counter.destroyed++;
  g_image_destroy_counter.destroy_image(device, image, allocator);
}

PFN_vkVoidFunction CountingGetInstanceProcAddr(VkInstance instance,
                                               const char* name) {
  PFN_vkVoidFunction proc =
      g_image_destroy_counter.get_instance_proc_addr(instance, name);
  if (proc != nullptr && std::strcmp(name, "vkDestroyImage") == 0) {
    g_image_destroy_counter.destroy_image =
        reinterpret_cast<PFN_vkDestroyImage>(proc);
    return reinterpret_cast<PFN_vkVoidFunction>(CountingDestroyImage);
  }
  return proc;
}

static_assert(std::is_same_v<decltype(&CountingGetInstanceProcAddr),
                             PFN_vkGetInstanceProcAddr>);
static_assert(
    std::is_same_v<decltype(&CountingDestroyImage), PFN_vkDestroyImage>);

void InstallImageDestroyCounter(EmbedderTestContextVulkan& context) {
  context.SetVulkanInstanceProcAddressCallback(
      [](void* user_data, FlutterVulkanInstanceHandle instance,
         const char* name) -> void* {
        if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) {
          g_image_destroy_counter.get_instance_proc_addr =
              reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                  EmbedderTestContextVulkan::InstanceProcAddr(user_data,
                                                              instance, name));
          return reinterpret_cast<void*>(CountingGetInstanceProcAddr);
        }
        return EmbedderTestContextVulkan::InstanceProcAddr(user_data, instance,
                                                           name);
      });
}

struct IdleReleaseCapture {
  fml::AutoResetWaitableEvent done;
  std::atomic<int> calls = 0;
  std::thread::id thread;
  FlutterAvioIdleResourceReleaseResult result = {};
  // Read on the raster thread when the callback runs.
  const std::atomic<int>* present_count = nullptr;
  int presents_at_callback = -1;
  size_t destroyed_images_at_callback = 0u;
};

void CaptureIdleRelease(const FlutterAvioIdleResourceReleaseResult* result,
                        void* user_data) {
  auto* capture = reinterpret_cast<IdleReleaseCapture*>(user_data);
  capture->thread = std::this_thread::get_id();
  capture->result = *result;
  if (capture->present_count != nullptr) {
    capture->presents_at_callback = capture->present_count->load();
  }
  capture->destroyed_images_at_callback =
      g_image_destroy_counter.destroyed.load();
  capture->calls++;
  capture->done.Signal();
}

FlutterAvioIdleResourceRelease MakeIdleRelease(
    FlutterAvioIdleResourceKinds kinds,
    IdleReleaseCapture* capture) {
  return FlutterAvioIdleResourceRelease{
      .struct_size = sizeof(FlutterAvioIdleResourceRelease),
      .kinds = kinds,
      .callback = capture ? CaptureIdleRelease : nullptr,
      .user_data = capture,
  };
}

// A running Vulkan Impeller engine that negotiated idle resource release and
// renders the selected-target scene `scene` (every view, for
// "render_all_views") into `target_count` backing stores.
class IdleReleaseFixture {
 public:
  IdleReleaseFixture(EmbedderTestContextVulkan& context,
                     FlutterAvioExtensionFeatures features,
                     const char* scene = "render_gradient_retained",
                     size_t target_count = 1u)
      : selected_target_(context.GetCompositor(), target_count) {
    EmbedderConfigBuilder builder(context);
    builder.AddCommandLineArgument("--enable-impeller");
    builder.SetDartEntrypoint("render_selected_target_ready");
    builder.AddDartEntrypointArgument(scene);
    context.AddNativeCallback(
        "SignalNativeTest",
        CREATE_NATIVE_ENTRY(
            [this](Dart_NativeArguments args) { dart_ready_.Signal(); }));
    builder.SetSurface(DlISize(800, 600));
    builder.SetRootRenderTargetCompositor(/*avoid_backing_store_cache=*/true,
                                          features);
    builder.SetRenderTargetType(
        EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);
    builder.GetProjectArgs().avio_resource_lifecycle_config = &resources_;
    builder.GetCompositor().user_data = &selected_target_;
    builder.GetCompositor().acquire_render_target_callback =
        AcquireSelectedTarget;
    builder.GetCompositor().collect_backing_store_callback =
        [](const FlutterBackingStore* store, void* user_data) {
          return reinterpret_cast<SelectedTargetTestContext*>(user_data)
              ->Collect(store);
        };
    builder.GetCompositor().present_render_target_callback =
        [](const FlutterPresentRenderTargetInfo* info) {
          return reinterpret_cast<SelectedTargetTestContext*>(info->user_data)
              ->Present(*info);
        };
    selected_target_.on_result =
        [this](const FlutterPresentRenderTargetInfo& info) {
          if (info.status == kFlutterPresentRenderTargetStatusPresented) {
            presents_++;
          } else {
            failed_results_++;
          }
          presented_.Signal();
          return true;
        };
    engine_ = builder.LaunchEngine();
  }

  // Waits once for the Dart entrypoint. Window metrics must follow it.
  bool WaitForDart() {
    if (dart_waited_) {
      return true;
    }
    if (!engine_.is_valid() ||
        dart_ready_.WaitWithTimeout(fml::TimeDelta::FromSeconds(5))) {
      return false;
    }
    dart_waited_ = true;
    return true;
  }

  bool RenderOneFrame() { return WaitForDart() && RenderFrame(1); }

  // Sends the implicit view's metrics, which schedules a frame, and waits
  // until `presents` more targets have been presented.
  bool RenderFrame(int presents) {
    const int expected = presents_.load() + presents;
    FlutterWindowMetricsEvent event = MakeMetrics(kFlutterImplicitViewId);
    if (FlutterEngineSendWindowMetricsEvent(engine_.get(), &event) !=
        kSuccess) {
      return false;
    }
    while (presents_.load() < expected) {
      if (failed_results_.load() != 0 ||
          presented_.WaitWithTimeout(fml::TimeDelta::FromSeconds(5))) {
        return false;
      }
    }
    return failed_results_.load() == 0;
  }

  // Adds a view the size of the implicit view (on the default display).
  bool AddView(FlutterViewId view_id) {
    struct AddResult {
      fml::AutoResetWaitableEvent done;
      bool added = false;
    };
    AddResult add_result;
    const FlutterWindowMetricsEvent metrics = MakeMetrics(view_id);
    FlutterAddViewInfo info = {};
    info.struct_size = sizeof(FlutterAddViewInfo);
    info.view_id = view_id;
    info.view_metrics = &metrics;
    info.user_data = &add_result;
    info.add_view_callback = [](const FlutterAddViewResult* result) {
      auto* add_result = reinterpret_cast<AddResult*>(result->user_data);
      add_result->added = result->added;
      add_result->done.Signal();
    };
    if (FlutterEngineAddView(engine_.get(), &info) != kSuccess) {
      return false;
    }
    return !add_result.done.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)) &&
           add_result.added;
  }

  const std::atomic<int>& present_count() const { return presents_; }

  FlutterEngine engine() const { return engine_.get(); }
  bool is_valid() const { return engine_.is_valid(); }
  void Shutdown() { engine_.reset(); }

 private:
  FlutterAvioResourceLifecycleConfig resources_ = {
      .struct_size = sizeof(FlutterAvioResourceLifecycleConfig),
      .transient_max_entries = 6u,
      .transient_max_bytes = 512u * 1024u * 1024u,
      .pipeline_cache_policy = kFlutterAvioPipelineCacheDisabled,
      .pipeline_cache_directory_fd = -1,
      .pipeline_cache_max_bytes = 0u,
  };
  static FlutterWindowMetricsEvent MakeMetrics(FlutterViewId view_id) {
    FlutterWindowMetricsEvent event = {};
    event.struct_size = sizeof(event);
    event.width = 800;
    event.height = 600;
    event.pixel_ratio = 1.0;
    event.view_id = view_id;
    return event;
  }

  SelectedTargetTestContext selected_target_;
  fml::AutoResetWaitableEvent dart_ready_;
  bool dart_waited_ = false;
  fml::AutoResetWaitableEvent presented_;
  std::atomic<int> presents_ = 0;
  std::atomic<int> failed_results_ = 0;
  UniqueEngine engine_;
};

}  // namespace

TEST_F(EmbedderTest, AvioIdleResourceReleaseRequiresNegotiation) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();
  IdleReleaseFixture fixture(
      context, kExactSelectedTargetFeatures |
                   kFlutterAvioExtensionFeatureResourceLifecycleConfig);
  ASSERT_TRUE(fixture.is_valid());
  IdleReleaseCapture capture;
  const auto request = MakeIdleRelease(0u, &capture);
  EXPECT_EQ(FlutterEngineReleaseAvioIdleResources(fixture.engine(), &request),
            kInternalInconsistency);
  fixture.Shutdown();
  EXPECT_EQ(capture.calls.load(), 0);
}

TEST_F(EmbedderTest, AvioIdleResourceReleaseRequiresResourceLifecycleConfig) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();
  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetSurface(DlISize(64, 64));
  builder.SetRootRenderTargetCompositor(
      /*avoid_backing_store_cache=*/true,
      kExactSelectedTargetFeatures |
          kFlutterAvioExtensionFeatureIdleResourceRelease);
  EXPECT_FALSE(builder.InitializeEngine().is_valid());
}

TEST_F(EmbedderTest, AvioIdleResourceReleaseRejectsMalformedRequests) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();
  IdleReleaseFixture fixture(context, kIdleReleaseFeatures);
  ASSERT_TRUE(fixture.is_valid());
  IdleReleaseCapture capture;

  auto request = MakeIdleRelease(0u, &capture);
  EXPECT_EQ(FlutterEngineReleaseAvioIdleResources(nullptr, &request),
            kInvalidArguments);
  EXPECT_EQ(FlutterEngineReleaseAvioIdleResources(fixture.engine(), nullptr),
            kInvalidArguments);
  request.struct_size = offsetof(FlutterAvioIdleResourceRelease, user_data);
  EXPECT_EQ(FlutterEngineReleaseAvioIdleResources(fixture.engine(), &request),
            kInvalidArguments);
  request = MakeIdleRelease(0x4u, &capture);
  EXPECT_EQ(FlutterEngineReleaseAvioIdleResources(fixture.engine(), &request),
            kInvalidArguments);
  fixture.Shutdown();
  EXPECT_EQ(capture.calls.load(), 0);
}

TEST_F(EmbedderTest, AvioIdleResourceReportOnlyChangesNothing) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();
  IdleReleaseFixture fixture(context, kIdleReleaseFeatures);
  ASSERT_TRUE(fixture.RenderOneFrame());

  IdleReleaseCapture first;
  const auto first_request = MakeIdleRelease(0u, &first);
  ASSERT_EQ(
      FlutterEngineReleaseAvioIdleResources(fixture.engine(), &first_request),
      kSuccess);
  ASSERT_FALSE(first.done.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  const auto& report = first.result;
  EXPECT_EQ(report.struct_size, sizeof(FlutterAvioIdleResourceReleaseResult));
  EXPECT_EQ(report.kinds_applied, 0u);
  EXPECT_EQ(report.transient_before.struct_size,
            sizeof(FlutterAvioRenderResourceUsage));
  // One root target was rendered: one multisampled set for its extent.
  EXPECT_EQ(report.transient_before.entries, 1u);
  EXPECT_EQ(report.transient_before.distinct_keys, 1u);
  EXPECT_EQ(report.transient_before.duplicate_entries, 0u);
  EXPECT_EQ(report.transient_before.created_entries, 1u);
  EXPECT_GT(report.transient_before.created_real_bytes, 0u);
  EXPECT_GT(report.transient_before.real_bytes, 0u);
  EXPECT_GT(report.transient_before.nominal_bytes, 0u);
  EXPECT_EQ(report.transient_after.entries, report.transient_before.entries);
  EXPECT_EQ(report.transient_after.real_bytes,
            report.transient_before.real_bytes);
  EXPECT_EQ(report.offscreen_after.entries, report.offscreen_before.entries);
  EXPECT_EQ(report.kept_in_use_entries, 0u);
  EXPECT_EQ(report.kept_recent_entries, 0u);

  // The interval restarted: nothing was created since the first report.
  IdleReleaseCapture second;
  const auto second_request = MakeIdleRelease(0u, &second);
  ASSERT_EQ(
      FlutterEngineReleaseAvioIdleResources(fixture.engine(), &second_request),
      kSuccess);
  ASSERT_FALSE(second.done.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  EXPECT_EQ(second.result.transient_before.entries, 1u);
  EXPECT_EQ(second.result.transient_before.created_entries, 0u);
  fixture.Shutdown();
}

// The request names no window: the engine's own recency keeps the warm set of
// a view that rendered moments ago.
TEST_F(EmbedderTest, AvioIdleResourceReleaseUsesEngineRecency) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();
  IdleReleaseFixture fixture(context, kIdleReleaseFeatures);
  ASSERT_TRUE(fixture.RenderOneFrame());

  IdleReleaseCapture capture;
  const auto request =
      MakeIdleRelease(kFlutterAvioIdleResourceTransientAttachments |
                          kFlutterAvioIdleResourceOffscreenTargets,
                      &capture);
  ASSERT_EQ(FlutterEngineReleaseAvioIdleResources(fixture.engine(), &request),
            kSuccess);
  ASSERT_FALSE(capture.done.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  EXPECT_EQ(capture.result.kinds_applied,
            kFlutterAvioIdleResourceTransientAttachments |
                kFlutterAvioIdleResourceOffscreenTargets);
  EXPECT_EQ(capture.result.transient_after.entries,
            capture.result.transient_before.entries);
  EXPECT_GE(
      capture.result.kept_in_use_entries + capture.result.kept_recent_entries,
      1u);
  fixture.Shutdown();
}

TEST_F(EmbedderTest, AvioIdleResourceReleaseCallsBackOnceOnRasterThread) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();
  IdleReleaseFixture fixture(context, kIdleReleaseFeatures);
  ASSERT_TRUE(fixture.RenderOneFrame());

  IdleReleaseCapture first;
  IdleReleaseCapture second;
  const auto first_request =
      MakeIdleRelease(kFlutterAvioIdleResourceTransientAttachments, &first);
  const auto second_request = MakeIdleRelease(0u, &second);
  ASSERT_EQ(
      FlutterEngineReleaseAvioIdleResources(fixture.engine(), &first_request),
      kSuccess);
  ASSERT_EQ(
      FlutterEngineReleaseAvioIdleResources(fixture.engine(), &second_request),
      kSuccess);
  // A request without a callback is accepted too.
  const auto silent_request = MakeIdleRelease(0u, nullptr);
  ASSERT_EQ(
      FlutterEngineReleaseAvioIdleResources(fixture.engine(), &silent_request),
      kSuccess);
  ASSERT_FALSE(first.done.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_FALSE(second.done.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  EXPECT_EQ(first.calls.load(), 1);
  EXPECT_EQ(second.calls.load(), 1);
  // Both ran on the raster task runner, not on the calling thread.
  EXPECT_NE(first.thread, std::this_thread::get_id());
  EXPECT_EQ(first.thread, second.thread);
  fixture.Shutdown();
  EXPECT_EQ(first.calls.load(), 1);
  EXPECT_EQ(second.calls.load(), 1);
}

// Patch 46a end to end: two same-sized views rendered in one frame share one
// transient attachment set. Red on the unpatched base (EN46a), where the
// second view's acquisition found the first view's set leased and in flight
// and allocated a second one.
TEST_F(EmbedderTest, AvioTwoSameSizeViewsInOneFrameUseOneTransientEntry) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();
  IdleReleaseFixture fixture(context, kIdleReleaseFeatures, "render_all_views",
                             /*target_count=*/2u);
  ASSERT_TRUE(fixture.WaitForDart());
  ASSERT_TRUE(fixture.AddView(123));
  // One frame renders the implicit view and view 123, both 800x600.
  ASSERT_TRUE(fixture.RenderFrame(/*presents=*/2));

  IdleReleaseCapture capture;
  const auto request = MakeIdleRelease(0u, &capture);
  ASSERT_EQ(FlutterEngineReleaseAvioIdleResources(fixture.engine(), &request),
            kSuccess);
  ASSERT_FALSE(capture.done.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  const auto& transients = capture.result.transient_before;
  EXPECT_EQ(transients.entries, 1u);
  EXPECT_EQ(transients.distinct_keys, 1u);
  EXPECT_EQ(transients.duplicate_entries, 0u);
  EXPECT_EQ(transients.created_entries, 1u);
  fixture.Shutdown();
}

// A release frees the memory before its raster task ends
// (ResourceManagerVK::Flush). The released set's images are destroyed before
// the callback runs, and the frame whose raster task was posted right after
// the request renders on a newly created set, never on the released one.
TEST_F(EmbedderTest, AvioReleaseIdleFreesBeforeNextRasterTask) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();
  InstallImageDestroyCounter(context);
  IdleReleaseFixture fixture(context, kIdleReleaseFeatures);
  ASSERT_TRUE(fixture.RenderOneFrame());
  // Let the one set age past the engine's recency window
  // (impeller::kIdleReleaseMinUnused, 5 s), so the release frees it.
  std::this_thread::sleep_for(std::chrono::milliseconds(5500));

  IdleReleaseCapture capture;
  capture.present_count = &fixture.present_count();
  const size_t destroyed_before = g_image_destroy_counter.destroyed.load();
  const int presents_before = fixture.present_count().load();
  const auto request =
      MakeIdleRelease(kFlutterAvioIdleResourceTransientAttachments, &capture);
  ASSERT_EQ(FlutterEngineReleaseAvioIdleResources(fixture.engine(), &request),
            kSuccess);
  // The next frame's raster task is posted after the release task.
  ASSERT_TRUE(fixture.RenderFrame(1));
  ASSERT_FALSE(capture.done.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));

  EXPECT_EQ(capture.result.transient_before.entries, 1u);
  EXPECT_EQ(capture.result.transient_after.entries, 0u);
  // The set's multisampled color and depth/stencil images were destroyed
  // inside the release's raster task, before its callback.
  EXPECT_GE(capture.destroyed_images_at_callback - destroyed_before, 2u);
  // The release ran before the next frame was presented.
  EXPECT_EQ(capture.presents_at_callback, presents_before);

  // That frame rendered on a newly created set.
  IdleReleaseCapture after;
  const auto report = MakeIdleRelease(0u, &after);
  ASSERT_EQ(FlutterEngineReleaseAvioIdleResources(fixture.engine(), &report),
            kSuccess);
  ASSERT_FALSE(after.done.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  EXPECT_EQ(after.result.transient_before.entries, 1u);
  EXPECT_EQ(after.result.transient_before.created_entries, 1u);
  fixture.Shutdown();
}

// The all-views-hidden trim (patch 36) still runs when idle release is
// negotiated, and it trims only transient attachments.
TEST_F(EmbedderTest, AvioHiddenEdgeTrimStillRunsWhenIdleReleaseNegotiated) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();
  IdleReleaseFixture fixture(
      context,
      kIdleReleaseFeatures | kFlutterAvioExtensionFeatureViewVisibility);
  ASSERT_TRUE(fixture.RenderOneFrame());

  IdleReleaseCapture before;
  const auto report = MakeIdleRelease(0u, &before);
  ASSERT_EQ(FlutterEngineReleaseAvioIdleResources(fixture.engine(), &report),
            kSuccess);
  ASSERT_FALSE(before.done.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(before.result.transient_before.entries, 1u);

  FlutterAvioViewVisibilityEvent hidden = {
      .struct_size = sizeof(FlutterAvioViewVisibilityEvent),
      .view_id = kFlutterImplicitViewId,
      .visibility = kFlutterAvioViewVisibilitySuspended,
  };
  ASSERT_EQ(FlutterEngineSetAvioViewVisibility(fixture.engine(), &hidden),
            kSuccess);
  // The hidden edge trims on the raster thread after a UI-thread hop. Poll
  // report-only requests until the trim has run.
  FlutterAvioIdleResourceReleaseResult after = {};
  for (int attempt = 0; attempt < 50; attempt++) {
    IdleReleaseCapture poll;
    const auto poll_request = MakeIdleRelease(0u, &poll);
    ASSERT_EQ(
        FlutterEngineReleaseAvioIdleResources(fixture.engine(), &poll_request),
        kSuccess);
    ASSERT_FALSE(poll.done.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
    after = poll.result;
    if (after.transient_before.entries == 0u) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_EQ(after.transient_before.entries, 0u);
  // The hidden edge is not extended to offscreen targets.
  EXPECT_EQ(after.offscreen_before.entries,
            before.result.offscreen_before.entries);
  fixture.Shutdown();
}

}  // namespace testing
}  // namespace flutter

// NOLINTEND(clang-analyzer-core.StackAddressEscape)
