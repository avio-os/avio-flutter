// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Exact-target conservation through the real engine: Dart, Animator,
// Rasterizer, EmbedderExternalViewEmbedder root-target delivery and the
// FrameOpportunityRegistry, driven through the public embedder ABI. Five views
// share display 1; the host grants each display baton with every view as a
// target, defers the next grant until all of them terminalize, returns the
// global lane's baton with the same targets (as an earlier host did; the
// global lane never renders them, so only display terminals are checked),
// refuses acquisitions and presents at random, and toggles visibility and
// scoped requests between grants. Every display target must reach exactly one
// terminal.

#define FML_USED_ON_EMBEDDER

#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "flutter/fml/synchronization/waitable_event.h"
#include "flutter/shell/platform/embedder/embedder.h"
#include "flutter/shell/platform/embedder/tests/embedder_config_builder.h"
#include "flutter/shell/platform/embedder/tests/embedder_test.h"
#include "flutter/shell/platform/embedder/tests/embedder_test_context_software.h"
#include "flutter/testing/testing.h"

namespace flutter {
namespace testing {
namespace {

constexpr FlutterEngineDisplayId kDisplay = 1;
const std::vector<FlutterViewId> kViews = {17, 18, 19, 20, 21};
constexpr size_t kDisplayGrants = 1000;

struct Host {
  std::mutex mutex;
  std::condition_variable changed;
  std::map<FlutterEngineDisplayId, intptr_t> batons;
  struct Open {
    FlutterEngineDisplayId display;
    std::set<FlutterViewId> pending;
    std::chrono::steady_clock::time_point granted_at;
  };
  std::map<FlutterFrameOpportunityId, Open> open;
  std::mt19937 rng{11};
  std::deque<std::string> log;
  size_t presented = 0;
  size_t refused_presents = 0;
  size_t refused_acquisitions = 0;
  size_t outcomes = 0;
  std::string violation;

  void Log(std::string line) {
    log.push_back(std::move(line));
    if (log.size() > 300) {
      log.pop_front();
    }
  }

  int Roll(int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(rng);
  }

  void Settle(FlutterFrameOpportunityId id,
              FlutterEngineDisplayId display,
              FlutterViewId target,
              const char* how) {
    auto found = open.find(id);
    if (found == open.end()) {
      violation = "terminal for unknown opportunity " + std::to_string(id);
      return;
    }
    if (found->second.display != display ||
        found->second.pending.erase(target) != 1) {
      violation = std::string("unexpected or second terminal ") + how + " " +
                  std::to_string(id) + "/" + std::to_string(target);
      return;
    }
    Log(std::string(how) + " " + std::to_string(id) + "/" +
        std::to_string(target));
  }
};

Host* g_host = nullptr;

std::string Join(const std::set<FlutterViewId>& ids) {
  std::string out;
  for (auto id : ids) {
    out += (out.empty() ? "" : ",") + std::to_string(id);
  }
  return "{" + out + "}";
}

void RunConservation(EmbedderTestContext& context) {
  Host host;
  g_host = &host;

  EmbedderConfigBuilder builder(context);
  builder.SetSurface(DlISize(800, 600));
  builder.SetDartEntrypoint("render_all_views");
  builder.SetRootRenderTargetCompositor(
      /*avoid_backing_store_cache=*/true,
      kFlutterAvioExtensionFeaturePerDisplayVsync |
          kFlutterAvioExtensionFeatureRootRenderTarget |
          kFlutterAvioExtensionFeatureExplicitRenderCompletion |
          kFlutterAvioExtensionFeatureExactVsyncCancellation |
          kFlutterAvioExtensionFeatureFrameOpportunityOutcomes |
          kFlutterAvioExtensionFeatureRenderDeadline |
          kFlutterAvioExtensionFeatureViewVisibility);
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kSoftwareBuffer);

  context.SetVsyncForDisplayCallback(
      [](intptr_t baton, FlutterEngineDisplayId display) {
        std::scoped_lock lock(g_host->mutex);
        g_host->Log("engine asks display " + std::to_string(display));
        g_host->batons[display] = baton;
        g_host->changed.notify_all();
      });
  builder.GetProjectArgs().vsync_for_display_callback =
      [](void* user_data, intptr_t baton, FlutterEngineDisplayId display) {
        reinterpret_cast<EmbedderTestContext*>(user_data)
            ->RunVsyncForDisplayCallback(baton, display);
      };
  builder.GetCompositor().frame_opportunity_outcome_callback =
      [](const FlutterFrameOpportunityOutcomeInfo* info) {
        std::scoped_lock lock(g_host->mutex);
        g_host->outcomes++;
        g_host->Settle(info->opportunity_id, info->display_id, info->target_id,
                       ("outcome" + std::to_string(info->outcome)).c_str());
        g_host->changed.notify_all();
      };
  builder.GetCompositor().acquire_render_target_callback =
      [](const FlutterRenderTargetAcquisitionInfo* info,
         FlutterBackingStore* backing_store_out) {
        int roll;
        {
          std::scoped_lock lock(g_host->mutex);
          roll = g_host->Roll(0, 99);
          if (roll >= 80) {
            g_host->refused_acquisitions++;
          }
        }
        if (roll < 80) {
          auto* compositor =
              reinterpret_cast<EmbedderTestCompositor*>(info->user_data);
          if (!compositor->CreateBackingStore(info->config,
                                              backing_store_out)) {
            return kFlutterRenderTargetAcquisitionBackpressured;
          }
          return kFlutterRenderTargetAcquisitionGranted;
        }
        if (roll < 84) {
          return kFlutterRenderTargetAcquisitionBackpressured;
        }
        if (roll < 88) {
          return kFlutterRenderTargetAcquisitionWithdrawn;
        }
        if (roll < 92) {
          return kFlutterRenderTargetAcquisitionRemoved;
        }
        if (roll < 96) {
          return kFlutterRenderTargetAcquisitionEpochStale;
        }
        return kFlutterRenderTargetAcquisitionHostRejected;
      };
  builder.GetCompositor().present_render_target_callback =
      [](const FlutterPresentRenderTargetInfo* info) {
        std::scoped_lock lock(g_host->mutex);
        const bool revision =
            info->status == kFlutterPresentRenderTargetStatusPresented ||
            info->status == kFlutterPresentRenderTargetStatusEmptyContent;
        if (revision && g_host->Roll(0, 19) == 0) {
          // A refused revision comes back from the engine as kHostRejected.
          g_host->refused_presents++;
          g_host->Log("host refuses present " +
                      std::to_string(info->opportunity_id) + "/" +
                      std::to_string(info->target_id));
          return false;
        }
        // Every other present callback is the target's terminal; the real
        // host forwards or faults any status it refuses.
        g_host->presented++;
        g_host->Settle(info->opportunity_id, info->display_id, info->target_id,
                       ("present" + std::to_string(info->status)).c_str());
        g_host->changed.notify_all();
        return true;
      };

  builder.GetCompositor().collect_backing_store_callback =
      [](const FlutterBackingStore* backing_store, void* user_data) {
        return reinterpret_cast<EmbedderTestCompositor*>(user_data)
            ->CollectBackingStore(backing_store);
      };
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());

  FlutterEngineDisplay display = {};
  display.struct_size = sizeof(FlutterEngineDisplay);
  display.display_id = kDisplay;
  display.single_display = true;
  display.refresh_rate = 60.0;
  display.width = 800;
  display.height = 600;
  display.device_pixel_ratio = 1.0;
  ASSERT_EQ(
      FlutterEngineNotifyDisplayUpdate(
          engine.get(), kFlutterEngineDisplaysUpdateTypeStartup, &display, 1),
      kSuccess);

  for (FlutterViewId view : kViews) {
    fml::AutoResetWaitableEvent added;
    FlutterWindowMetricsEvent metrics = {};
    metrics.struct_size = sizeof(FlutterWindowMetricsEvent);
    metrics.width = 96;
    metrics.height = 64;
    metrics.pixel_ratio = 1.0;
    metrics.display_id = kDisplay;
    metrics.view_id = view;
    FlutterAddViewInfo add_view = {};
    add_view.struct_size = sizeof(FlutterAddViewInfo);
    add_view.view_id = view;
    add_view.view_metrics = &metrics;
    add_view.user_data = &added;
    add_view.add_view_callback = [](const FlutterAddViewResult* result) {
      EXPECT_TRUE(result->added);
      reinterpret_cast<fml::AutoResetWaitableEvent*>(result->user_data)
          ->Signal();
    };
    ASSERT_EQ(FlutterEngineAddView(engine.get(), &add_view), kSuccess);
    added.Wait();
  }

  FlutterFrameOpportunityId next_id = 5000;
  size_t display_grants = 0;
  size_t global_grants = 0;
  std::set<FlutterViewId> hidden;
  auto grant = [&](FlutterEngineDisplayId lane, intptr_t baton) {
    const FlutterFrameOpportunityId id = next_id++;
    std::vector<FlutterViewId> targets(kViews.begin(), kViews.end());
    host.open[id] = Host::Open{
        .display = lane,
        .pending = std::set<FlutterViewId>(targets.begin(), targets.end()),
        .granted_at = std::chrono::steady_clock::now()};
    host.Log("host grants " + std::to_string(id) + " lane " +
             std::to_string(lane));
    const uint64_t now = FlutterEngineGetCurrentTime();
    const auto result = FlutterEngineOnVsyncForDisplayWithOpportunity(
        engine.get(), baton, lane, id, targets.data(), targets.size(), now,
        now + 4'000'000, now + 8'000'000);
    if (result != kSuccess) {
      host.open.erase(id);
      host.Log("  refused");
    }
  };

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(60);
  std::unique_lock lock(host.mutex);
  int idle_waits = 0;
  while (std::chrono::steady_clock::now() < deadline &&
         display_grants < kDisplayGrants) {
    ASSERT_TRUE(host.violation.empty()) << host.violation;
    bool display_open = false;
    for (const auto& [id, open] : host.open) {
      if (open.display == kDisplay && !open.pending.empty()) {
        display_open = true;
        const auto age = std::chrono::steady_clock::now() - open.granted_at;
        if (age > std::chrono::seconds(3)) {
          std::ostringstream trace;
          for (const auto& line : host.log) {
            trace << "  " << line << "\n";
          }
          ADD_FAILURE() << "display opportunity " << id << " still owes "
                        << Join(open.pending) << " after 3 s\n"
                        << trace.str();
          return;
        }
      }
    }
    auto display_baton = host.batons.find(kDisplay);
    if (display_baton != host.batons.end() && !display_open) {
      const intptr_t baton = display_baton->second;
      host.batons.erase(display_baton);
      grant(kDisplay, baton);
      display_grants++;
      idle_waits = 0;
      continue;
    }
    auto global_baton = host.batons.find(0);
    if (global_baton != host.batons.end()) {
      const intptr_t baton = global_baton->second;
      host.batons.erase(global_baton);
      grant(0, baton);
      global_grants++;
      continue;
    }
    // Host events between grants: scoped requests (descriptor-driven Dart
    // rebuilds), unscoped requests and visibility changes.
    const int roll = host.Roll(0, 9);
    const FlutterViewId view = kViews[host.Roll(0, kViews.size() - 1)];
    lock.unlock();
    if (roll < 6) {
      FlutterEngineScheduleFrameForDisplayViewsWithRequestKind(
          engine.get(), kDisplay, &view, 1,
          roll < 4 ? kFlutterEngineFrameRequestKindRebuild
                   : kFlutterEngineFrameRequestKindReuseLatest);
    } else if (roll < 8) {
      FlutterEngineScheduleFrame(engine.get());
    } else {
      const bool hide = hidden.count(view) == 0;
      if (hide) {
        hidden.insert(view);
      } else {
        hidden.erase(view);
      }
      FlutterAvioViewVisibilityEvent event = {};
      event.struct_size = sizeof(event);
      event.view_id = view;
      event.visibility = hide ? kFlutterAvioViewVisibilityObscured
                              : kFlutterAvioViewVisibilityVisible;
      FlutterEngineSetAvioViewVisibility(engine.get(), &event);
    }
    lock.lock();
    if (host.changed.wait_for(lock, std::chrono::milliseconds(20)) ==
        std::cv_status::timeout) {
      idle_waits++;
    }
  }
  ASSERT_TRUE(host.violation.empty()) << host.violation;

  // Drain: no new demand; the last display grant must settle.
  const auto settle_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < settle_deadline) {
    bool owed = false;
    for (const auto& [id, open] : host.open) {
      owed = owed || (open.display == kDisplay && !open.pending.empty());
    }
    if (!owed) {
      break;
    }
    host.changed.wait_for(lock, std::chrono::milliseconds(50));
  }
  size_t stranded_global = 0;
  for (const auto& [id, open] : host.open) {
    if (open.display == kDisplay) {
      EXPECT_TRUE(open.pending.empty())
          << "display opportunity " << id << " strands " << Join(open.pending);
    } else if (!open.pending.empty()) {
      stranded_global++;
    }
  }
  std::cout << "software root target: " << display_grants << " display grants, "
            << global_grants << " global-lane grants (" << stranded_global
            << " left owing), " << host.presented << " present terminals, "
            << host.outcomes << " outcome terminals, "
            << host.refused_acquisitions << " refused acquisitions, "
            << host.refused_presents << " refused presents" << std::endl;
  EXPECT_EQ(display_grants, kDisplayGrants);
  lock.unlock();
  g_host = nullptr;
}

TEST_F(EmbedderTest, AvioExactTargetsTerminalizeThroughRealRaster) {
  RunConservation(GetEmbedderContext<EmbedderTestContextSoftware>());
}

}  // namespace
}  // namespace testing
}  // namespace flutter
