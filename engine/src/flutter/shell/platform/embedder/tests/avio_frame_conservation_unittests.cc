// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Frame-opportunity conservation for several views on one display.
//
// Drives the real Animator, the real embedder vsync waiter (two posted hops
// between a baton's return and OnDisplayVsync) and the real
// FrameOpportunityRegistry. The UI queue is stepped one task at a time from a
// fixed seed, so host and framework events interleave inside every window the
// engine leaves open. Raster is a FIFO of the delegate's draw tasks in which
// every layer-tree task reaches exactly one terminal (the Rasterizer
// contract), so a display target still owed after the engine quiesces was
// lost between admission and raster. With display churn the harness also
// requires every visible view the framework dirtied to have been built.

#define FML_USED_ON_EMBEDDER

#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "flutter/common/frame_opportunity.h"
#include "flutter/flow/frame_timings.h"
#include "flutter/flow/layers/layer_tree.h"
#include "flutter/fml/message_loop.h"
#include "flutter/fml/message_loop_impl.h"
#include "flutter/fml/task_runner.h"
#include "flutter/shell/common/animator.h"
#include "flutter/shell/common/pipeline.h"
#include "flutter/shell/platform/embedder/vsync_waiter_embedder.h"
#include "gtest/gtest.h"

namespace flutter {
namespace testing {
namespace {

class SteppedTaskRunner final : public fml::TaskRunner {
 public:
  explicit SteppedTaskRunner(fml::TaskQueueId queue_id)
      : fml::TaskRunner(nullptr), queue_id_(queue_id) {}

  void PostTask(const fml::closure& task) override { tasks_.push_back(task); }
  void PostTaskForTime(const fml::closure& task, fml::TimePoint) override {
    tasks_.push_back(task);
  }
  void PostDelayedTask(const fml::closure& task, fml::TimeDelta) override {
    // Only the animator's idle notification is delayed; it never affects
    // frame custody.
  }
  bool RunsTasksOnCurrentThread() override { return true; }
  fml::TaskQueueId GetTaskQueueId() override { return queue_id_; }

  bool RunOne() {
    if (tasks_.empty()) {
      return false;
    }
    auto task = std::move(tasks_.front());
    tasks_.pop_front();
    task();
    return true;
  }
  void Clear() { tasks_.clear(); }

 private:
  fml::TaskQueueId queue_id_;
  std::deque<fml::closure> tasks_;
};

struct OpenOpportunity {
  int64_t display_id;
  std::set<int64_t> pending;
};

struct Options {
  // The global lane's baton comes back carrying the display's targets, as an
  // earlier host returned it. The global lane never renders them.
  bool global_lane_lent_display_targets = true;
  bool immediate_frames = false;
  bool visibility = true;
  // View removal and re-add, same-set display updates and host cancellation.
  bool topology = false;
  // The display leaving and returning, the host cancelling the baton it held
  // for it, and re-homing views before or after the display returns. The host
  // publishes every target it has had, re-requests after a cancellation, and
  // every visible view the framework dirtied must have been built.
  bool display_churn = false;
  int steps = 400;
  uint32_t first_seed = 1;
};

struct Coverage {
  size_t display_grants = 0;
  size_t global_grants = 0;
  size_t retained_redraws = 0;
  size_t all_views_rendered = 0;
  size_t raster_items = 0;
  size_t topology_events = 0;
  size_t display_returns = 0;
  size_t cancellations = 0;
  std::map<int, size_t> outcomes;
  size_t presented = 0;
  size_t stranded_global = 0;
  void Add(const Coverage& other) {
    display_grants += other.display_grants;
    global_grants += other.global_grants;
    retained_redraws += other.retained_redraws;
    all_views_rendered += other.all_views_rendered;
    raster_items += other.raster_items;
    topology_events += other.topology_events;
    display_returns += other.display_returns;
    cancellations += other.cancellations;
    for (const auto& [outcome, count] : other.outcomes) {
      outcomes[outcome] += count;
    }
    presented += other.presented;
    stranded_global += other.stranded_global;
  }
  std::string Describe() const {
    std::ostringstream out;
    out << "display grants " << display_grants << ", global grants "
        << global_grants << ", raster items " << raster_items
        << ", retained redraws " << retained_redraws << ", OnAllViewsRendered "
        << all_views_rendered << ", presented " << presented
        << ", topology events " << topology_events << ", display returns "
        << display_returns << ", cancellations " << cancellations
        << ", stranded global-lane targets " << stranded_global << ", outcomes";
    for (const auto& [outcome, count] : outcomes) {
      out << " [" << outcome << "]=" << count;
    }
    return out.str();
  }
};

class Harness final : public Animator::Delegate {
 public:
  static constexpr int64_t kDisplay = 1;
  static constexpr int64_t kImplicitView = 0;

  Harness(uint32_t seed, Options options)
      : rng_(seed),
        options_(options),
        registry_([this](auto... args) { OnRegistryOutcome(args...); }) {
    fml::MessageLoop::EnsureInitializedForCurrentThread();
    const auto queue = fml::MessageLoop::GetCurrentTaskQueueId();
    ui_ = fml::MakeRefCounted<SteppedTaskRunner>(queue);
    raster_ = fml::MakeRefCounted<SteppedTaskRunner>(queue);
    platform_ = fml::MakeRefCounted<SteppedTaskRunner>(queue);
    io_ = fml::MakeRefCounted<SteppedTaskRunner>(queue);
    TaskRunners runners("conservation", platform_, raster_, ui_, io_);
    auto waiter = std::make_unique<VsyncWaiterEmbedder>(
        [this](intptr_t baton) { batons_[kImplicitView] = baton; },
        [this](intptr_t baton, int64_t display) {
          Log("engine asks display " + std::to_string(display) + " baton " +
              std::to_string(baton));
          batons_[display] = baton;
        },
        runners);
    waiter_ = waiter.get();
    animator_ = std::make_unique<Animator>(*this, runners, std::move(waiter));

    animator_->AddDisplay(kDisplay, 60.0);
    EXPECT_TRUE(animator_->RegisterView(kImplicitView));
    for (int64_t view : kDisplayViews) {
      EXPECT_TRUE(animator_->RegisterInitialViewDisplay(view, kDisplay));
    }
    metrics_views_ = {kImplicitView};
    metrics_views_.insert(kDisplayViews.begin(), kDisplayViews.end());
    for (int64_t view : metrics_views_) {
      dirty_.insert(view);
    }
    animator_->RequestFrameForDisplay(kDisplay);
    animator_->RequestFrame();
  }

  ~Harness() {
    ui_->Clear();
    raster_->Clear();
    animator_.reset();
  }

  // |Animator::Delegate|
  void OnAnimatorBeginFrame(fml::TimePoint, uint64_t) override {
    Log("dart global frame");
    rendered_during_frame_.clear();  // RuntimeController::BeginFrame
    const std::set<int64_t> to_render = dirty_;
    for (int64_t view : to_render) {
      RenderView(view);
    }
    MaybeTick();
  }

  // |Animator::Delegate|
  void OnAnimatorBeginFrameForDisplay(
      fml::TimePoint,
      uint64_t,
      int64_t display_id,
      const std::set<int64_t>& view_ids) override {
    Log("dart display " + std::to_string(display_id) + " frame views " +
        Join(view_ids));
    // RuntimeController::BeginFrameForDisplay
    rendered_during_frame_.clear();
    std::set<int64_t> to_render;
    for (int64_t view : view_ids) {
      if (dirty_.count(view) > 0) {
        to_render.insert(view);
      }
    }
    for (int64_t view : to_render) {
      RenderView(view);
    }
    MaybeTick();
  }

  // |Animator::Delegate|
  void OnAnimatorNotifyIdle(fml::TimeDelta) override {}

  // |Animator::Delegate|
  void OnAnimatorUpdateLatestFrameTargetTime(fml::TimePoint) override {}

  // |Animator::Delegate|
  void OnAnimatorDraw(std::shared_ptr<FramePipeline> pipeline) override {
    raster_->PostTask([this, pipeline] { DrawPipeline(pipeline); });
  }

  // |Animator::Delegate|
  void OnAnimatorDrawLastLayerTrees(
      std::unique_ptr<FrameTimingsRecorder> recorder) override {
    PostRetainedRedraw(std::move(recorder), painted_);
  }

  // |Animator::Delegate|
  void OnAnimatorDrawLastLayerTreesForDisplay(
      std::unique_ptr<FrameTimingsRecorder> recorder,
      const std::set<int64_t>& view_ids) override {
    PostRetainedRedraw(std::move(recorder), view_ids);
  }

  // |Animator::Delegate|
  void OnAnimatorEmptyFrameForDisplay(int64_t,
                                      const std::set<int64_t>&) override {}

  // |Animator::Delegate|
  bool OnAnimatorFrameOpportunityOutcome(
      FrameOpportunityId id,
      int64_t display_id,
      int64_t target_id,
      FrameOpportunityOutcome outcome) override {
    return registry_.Complete(id, display_id, target_id, outcome);
  }

  void Step() {
    if (options_.topology && Uniform(0, 49) == 0) {
      TopologyEvent();
      return;
    }
    const int roll = Uniform(0, 99);
    if (roll < 30) {
      if (ui_->RunOne()) {
        Log("ui task");
      }
    } else if (roll < 45) {
      if (raster_->RunOne()) {
        Log("raster task");
      }
    } else if (roll < 60) {
      GrantDisplay();
    } else if (roll < 66) {
      ReturnGlobalLane();
    } else if (roll < 80) {
      MarkDirtyAndSchedule(RandomView());
    } else if (roll < 82) {
      Log("unscoped scheduleFrame");
      animator_->RequestFrame();
    } else if (roll < 85) {
      // MarkTextureFrameAvailable: a texture-only frame reuses retained trees.
      if (Uniform(0, 1) == 0) {
        Log("texture frame for display");
        animator_->RequestFrameForDisplay(kDisplay, false);
      } else {
        Log("texture frame unscoped");
        animator_->RequestFrame(false);
      }
    } else if (roll < 89) {
      Log("pointer secondary vsync");
      animator_->ScheduleSecondaryVsyncCallback(
          7, [this] { Log("secondary callback"); });
    } else if (roll < 95) {
      if (options_.visibility) {
        ToggleVisibility(RandomDisplayView());
      }
    } else {
      if (options_.immediate_frames) {
        Log("immediate configure frame");
        animator_->ScheduleImmediateFrame(++configure_serial_);
      }
    }
  }

  // Lets every queued engine task, raster task and owed host grant run, with
  // no new framework demand, until nothing moves. A display that left comes
  // back first, as the display list settles.
  void Quiesce() {
    if (!display_present_) {
      DisplayReturns(/*rehome_first=*/false);
    }
    ticks_enabled_ = false;
    for (int round = 0; round < 10000; round++) {
      bool moved = false;
      while (ui_->RunOne()) {
        moved = true;
      }
      while (raster_->RunOne()) {
        moved = true;
      }
      moved = GrantDisplay() || moved;
      moved = ReturnGlobalLane() || moved;
      if (!moved) {
        return;
      }
    }
    ADD_FAILURE() << "engine never quiesced";
  }

  // Display targets still owed after quiescence. The host defers the
  // display's next grant behind them, so each one is a frozen display lane.
  std::map<FrameOpportunityId, std::set<int64_t>> StrandedDisplayTargets()
      const {
    return OwedTargets(kDisplay);
  }

  // Visible, registered display views the framework dirtied that were never
  // built. The framework waits for its scheduled frame, so each one is a view
  // that never draws again.
  std::set<int64_t> UnbuiltDirtyViews() const {
    std::set<int64_t> unbuilt;
    for (int64_t view : kDisplayViews) {
      if (dirty_.count(view) > 0 && removed_.count(view) == 0 &&
          hidden_.count(view) == 0) {
        unbuilt.insert(view);
      }
    }
    return unbuilt;
  }

  std::string Trace(size_t last = 160) const {
    std::ostringstream out;
    const size_t start = log_.size() > last ? log_.size() - last : 0;
    for (size_t i = start; i < log_.size(); i++) {
      out << "  " << i << ": " << log_[i] << "\n";
    }
    return out.str();
  }

  Coverage coverage() {
    coverage_.stranded_global = OwedTargets(kImplicitView).size();
    return coverage_;
  }

 private:
  static inline const std::vector<int64_t> kDisplayViews = {17, 18, 19, 20, 21};

  int Uniform(int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(rng_);
  }
  int64_t RandomDisplayView() {
    return kDisplayViews[Uniform(0, kDisplayViews.size() - 1)];
  }
  int64_t RandomView() {
    return Uniform(0, 5) == 0 ? kImplicitView : RandomDisplayView();
  }

  static std::string Join(const std::set<int64_t>& ids) {
    std::string out = "{";
    for (int64_t id : ids) {
      if (out.size() > 1) {
        out += ",";
      }
      out += std::to_string(id);
    }
    return out + "}";
  }

  void Log(std::string line) { log_.push_back(std::move(line)); }

  std::map<FrameOpportunityId, std::set<int64_t>> OwedTargets(
      int64_t lane) const {
    std::map<FrameOpportunityId, std::set<int64_t>> owed;
    for (const auto& [id, open] : open_) {
      if (open.display_id == lane && !open.pending.empty()) {
        owed[id] = open.pending;
      }
    }
    return owed;
  }

  void TopologyEvent() {
    coverage_.topology_events++;
    const int kind = Uniform(0, options_.display_churn ? 4 : 3);
    if (kind <= 1) {
      const int64_t view = RandomDisplayView();
      if (removed_.count(view) == 0) {
        Log("engine removes view " + std::to_string(view));
        animator_->RemoveView(view);
        removed_.insert(view);
        dirty_.erase(view);
        metrics_views_.erase(view);
        hidden_.erase(view);
      } else {
        Log("engine adds view " + std::to_string(view));
        EXPECT_TRUE(animator_->RegisterInitialViewDisplay(view, kDisplay));
        removed_.erase(view);
        metrics_views_.insert(view);
        MarkDirtyAndSchedule(view);
      }
    } else if (kind == 2) {
      if (display_present_) {
        Log("engine display update with the same display set");
        animator_->RemoveStaleDisplays({kDisplay});
        animator_->AddDisplay(kDisplay, 60.0);
      }
    } else if (kind == 3) {
      CancelOpenDisplayOpportunity();
    } else if (display_present_) {
      DisplayLeaves();
    } else {
      DisplayReturns(/*rehome_first=*/Uniform(0, 1) == 0);
    }
  }

  // Engine::SetDisplays without the display. The host may cancel the baton it
  // holds for it, through the transport or as a removed target.
  void DisplayLeaves() {
    Log("engine display update without display " + std::to_string(kDisplay));
    display_present_ = false;
    animator_->RemoveStaleDisplays({});
    animator_->RequestFrame();
    auto baton = batons_.find(kDisplay);
    if (baton == batons_.end()) {
      return;
    }
    const int how = Uniform(0, 2);
    if (how == 0) {
      return;  // The host returns it later, like any grant.
    }
    const auto reason = how == 1
                            ? VsyncWaiter::CancellationReason::kTargetRemoved
                            : VsyncWaiter::CancellationReason::kTransportLost;
    Log(std::string("host cancels the display baton: ") +
        (how == 1 ? "target removed" : "transport lost"));
    const intptr_t value = baton->second;
    batons_.erase(baton);
    coverage_.cancellations++;
    EXPECT_TRUE(waiter_->CancelVsync(kDisplay, value, reason, nullptr));
  }

  // Engine::SetDisplays with the display again, the embedder re-homing every
  // registered view on it before or after the display list arrives.
  void DisplayReturns(bool rehome_first) {
    Log(std::string("display returns, views re-homed ") +
        (rehome_first ? "first" : "after"));
    display_present_ = true;
    coverage_.display_returns++;
    if (!rehome_first) {
      animator_->RemoveStaleDisplays({kDisplay});
      animator_->AddDisplay(kDisplay, 60.0);
    }
    for (int64_t view : kDisplayViews) {
      if (removed_.count(view) == 0) {
        animator_->SetViewDisplay(view, kDisplay);
      }
    }
    if (rehome_first) {
      animator_->RemoveStaleDisplays({kDisplay});
      animator_->AddDisplay(kDisplay, 60.0);
    }
    animator_->RequestFrame();
  }

  // EmbedderEngine::CancelFrameOpportunity. With display churn the host's
  // replacement epoch asks for the display again once the cancellation has
  // settled on the UI thread.
  void CancelOpenDisplayOpportunity() {
    for (const auto& [id, open] : open_) {
      if (open.display_id != kDisplay || open.pending.empty()) {
        continue;
      }
      coverage_.cancellations++;
      const FrameOpportunityId cancelled = id;
      VsyncWaiter::CancellationReason reason =
          VsyncWaiter::CancellationReason::kEpochReplaced;
      Log("host cancels opportunity " + std::to_string(cancelled));
      if (!registry_.Cancel(cancelled, kDisplay,
                            FrameOpportunityOutcome::kEpochStale)) {
        return;
      }
      fml::closure rerequest = nullptr;
      if (options_.display_churn) {
        rerequest = [this] {
          Log("host epoch asks for the display");
          animator_->RequestFrameForDisplay(kDisplay);
        };
      }
      if (waiter_->CancelFrameOpportunity(kDisplay, cancelled, reason,
                                          rerequest)) {
        return;
      }
      ui_->PostTask([this, cancelled, reason, rerequest] {
        animator_->CancelFrameOpportunity(kDisplay, cancelled, reason);
        if (rerequest) {
          rerequest();
        }
      });
      return;
    }
  }

  void RenderView(int64_t view) {
    Log("dart renders " + std::to_string(view));
    animator_->Render(
        view, std::make_unique<LayerTree>(nullptr, DlISize(64, 64)), 1.0);
    dirty_.erase(view);  // The framework's view is clean once it rendered.
    // RuntimeController::CheckIfAllViewsRendered
    rendered_during_frame_.insert(view);
    if (rendered_during_frame_.size() == metrics_views_.size()) {
      Log("runtime: all views rendered");
      coverage_.all_views_rendered++;
      animator_->OnAllViewsRendered();
      rendered_during_frame_.clear();
    }
  }

  void MaybeTick() {
    if (ticks_enabled_ && Uniform(0, 4) == 0) {
      Log("ticker during frame");
      MarkDirtyAndSchedule(RandomView());
    }
  }

  void MarkDirtyAndSchedule(int64_t view) {
    if (!ticks_enabled_ || removed_.count(view) > 0) {
      return;
    }
    dirty_.insert(view);
    if (view == kImplicitView) {
      Log("dirty 0, scheduleFrame");
      animator_->RequestFrame();
    } else {
      const bool retained =
          animator_->RequestFrameForDisplayViews(kDisplay, {view});
      Log("dirty " + std::to_string(view) +
          ", scoped request retained=" + std::to_string(retained));
    }
  }

  void ToggleVisibility(int64_t view) {
    if (removed_.count(view) > 0) {
      return;
    }
    const bool hide = hidden_.count(view) == 0;
    if (hide) {
      hidden_.insert(view);
    } else {
      hidden_.erase(view);
    }
    Log(std::string(hide ? "host hides " : "host shows ") +
        std::to_string(view));
    animator_->SetViewVisibility(view,
                                 hide ? Animator::ViewVisibility::kObscured
                                      : Animator::ViewVisibility::kVisible);
  }

  // The host grants a display's next baton only after its previous grant
  // terminalized, naming its published targets. Those can lag the engine's
  // views: a removed view may still be named. With display churn the host
  // names every target it has had.
  bool GrantDisplay() {
    auto baton = batons_.find(kDisplay);
    if (baton == batons_.end()) {
      return false;
    }
    if (!OwedTargets(kDisplay).empty()) {
      return false;
    }
    std::vector<int64_t> targets;
    for (int64_t view : kDisplayViews) {
      if (options_.display_churn || Uniform(0, 1) == 0 ||
          removed_.count(view) == 0) {
        targets.push_back(view);
      }
    }
    if (targets.empty()) {
      return false;
    }
    const FrameOpportunityId id = next_opportunity_++;
    const intptr_t value = baton->second;
    batons_.erase(baton);
    ReturnWithOpportunity(kDisplay, value, id, targets);
    coverage_.display_grants++;
    return true;
  }

  bool ReturnGlobalLane() {
    auto baton = batons_.find(kImplicitView);
    if (baton == batons_.end()) {
      return false;
    }
    const intptr_t value = baton->second;
    batons_.erase(baton);
    coverage_.global_grants++;
    if (options_.global_lane_lent_display_targets) {
      std::vector<int64_t> targets(kDisplayViews.begin(), kDisplayViews.end());
      ReturnWithOpportunity(kImplicitView, value, next_opportunity_++, targets);
    } else {
      const auto now = fml::TimePoint::Now();
      Log("host returns global baton without targets");
      EXPECT_TRUE(waiter_->ReturnVsync(
          kImplicitView, value, now, now + fml::TimeDelta::FromMilliseconds(2),
          std::nullopt));
    }
    return true;
  }

  // EmbedderEngine::OnVsyncEventForDisplayWithOpportunity
  void ReturnWithOpportunity(int64_t display,
                             intptr_t baton,
                             FrameOpportunityId id,
                             const std::vector<int64_t>& targets) {
    ASSERT_TRUE(registry_.Open(id, display, targets));
    open_[id] = OpenOpportunity{
        .display_id = display,
        .pending = std::set<int64_t>(targets.begin(), targets.end())};
    Log("host grants opportunity " + std::to_string(id) + " display " +
        std::to_string(display) + " targets " + Join(open_[id].pending));
    const auto now = fml::TimePoint::Now();
    if (!waiter_->ReturnVsync(
            display, baton, now, now + fml::TimeDelta::FromMilliseconds(2), id,
            open_[id].pending, now + fml::TimeDelta::FromMilliseconds(1))) {
      ASSERT_TRUE(registry_.Abandon(id, display));
      open_.erase(id);
      Log("  waiter refused; abandoned");
    }
  }

  void OnRegistryOutcome(FrameOpportunityId id,
                         int64_t display,
                         int64_t target,
                         FrameOpportunityOutcome outcome) {
    Log("terminal " + std::to_string(id) + "/" + std::to_string(target) +
        " outcome " + std::to_string(static_cast<int>(outcome)));
    coverage_.outcomes[static_cast<int>(outcome)]++;
    Settle(id, display, target);
  }

  void Settle(FrameOpportunityId id, int64_t display, int64_t target) {
    auto open = open_.find(id);
    ASSERT_NE(open, open_.end()) << "terminal for unknown opportunity " << id;
    ASSERT_EQ(open->second.display_id, display);
    ASSERT_EQ(open->second.pending.erase(target), 1u)
        << "second terminal for " << id << "/" << target;
  }

  // Root delivery: claim, then the host's present callback is the terminal.
  void Present(const FrameOpportunityContext& opportunity, int64_t target) {
    if (registry_.ClaimTarget(opportunity.id, opportunity.display_id, target)) {
      Log("presented " + std::to_string(opportunity.id) + "/" +
          std::to_string(target));
      coverage_.presented++;
      Settle(opportunity.id, opportunity.display_id, target);
    }
  }

  void DrawPipeline(const std::shared_ptr<FramePipeline>& pipeline) {
    const auto consumed =
        pipeline->Consume([this](std::unique_ptr<FrameItem> item) {
          const auto opportunity =
              item->frame_timings_recorder->GetFrameOpportunity();
          std::set<int64_t> views;
          for (const auto& task : item->layer_tree_tasks) {
            views.insert(task->view_id);
          }
          Log("raster item opportunity " +
              std::to_string(opportunity ? opportunity->id : 0) + " views " +
              Join(views));
          coverage_.raster_items++;
          std::set<int64_t> retry;
          std::vector<std::unique_ptr<LayerTreeTask>> resubmitted;
          for (auto& task : item->layer_tree_tasks) {
            if (!opportunity.has_value()) {
              // No exact target can be acquired without an opportunity.
              continue;
            }
            if (Uniform(0, 29) == 0) {
              // DrawSurfaceStatus::kRetry: the task goes back to the front.
              resubmitted.push_back(std::move(task));
              continue;
            }
            const int roll = Uniform(0, 19);
            if (roll < 16) {
              painted_.insert(task->view_id);
              Present(*opportunity, task->view_id);
            } else if (roll < 18) {
              registry_.Complete(opportunity->id, opportunity->display_id,
                                 task->view_id,
                                 FrameOpportunityOutcome::kNoVisualChange);
            } else if (registry_.Complete(
                           opportunity->id, opportunity->display_id,
                           task->view_id,
                           FrameOpportunityOutcome::kBackpressured)) {
              retry.insert(task->view_id);
            }
          }
          if (!resubmitted.empty()) {
            // Rasterizer::Draw's front resubmission.
            auto again = std::make_unique<FrameItem>(
                std::move(resubmitted),
                item->frame_timings_recorder->CloneUntil(
                    FrameTimingsRecorder::State::kBuildEnd));
            Log("raster resubmits");
            resubmit_ = std::move(again);
          }
          if (opportunity.has_value() && !retry.empty()) {
            RearmBackpressured(opportunity->display_id, retry);
          }
        });
    auto more = consumed;
    if (resubmit_) {
      auto front =
          pipeline->ProduceIfEmpty([this](std::unique_ptr<FrameItem> rejected) {
            BackpressureItem(*rejected);
          });
      if (!front) {
        BackpressureItem(*resubmit_);
      } else if (front.Complete(std::move(resubmit_)).success) {
        more = PipelineConsumeResult::MoreAvailable;
      }
      resubmit_.reset();
    }
    if (more == PipelineConsumeResult::MoreAvailable) {
      raster_->PostTask([this, pipeline] { DrawPipeline(pipeline); });
    }
  }

  // Rasterizer::CompleteBackpressuredFrameItem
  void BackpressureItem(const FrameItem& item) {
    const auto opportunity = item.frame_timings_recorder->GetFrameOpportunity();
    std::set<int64_t> retry;
    for (const auto& task : item.layer_tree_tasks) {
      if (opportunity.has_value() &&
          registry_.Complete(opportunity->id, opportunity->display_id,
                             task->view_id,
                             FrameOpportunityOutcome::kBackpressured)) {
        retry.insert(task->view_id);
      }
    }
    if (opportunity.has_value() && !retry.empty()) {
      RearmBackpressured(opportunity->display_id, retry);
    }
  }

  void RearmBackpressured(int64_t display, std::set<int64_t> retry) {
    ui_->PostTask([this, display, retry] {
      Log("rearm backpressured " + Join(retry));
      static_cast<void>(animator_->RequestFrameForDisplayViews(
          display, retry, /*regenerate_layer_trees=*/true));
    });
  }

  void PostRetainedRedraw(std::unique_ptr<FrameTimingsRecorder> recorder,
                          std::set<int64_t> views) {
    auto opportunity = recorder->GetFrameOpportunity();
    raster_->PostTask([this, opportunity, views] {
      Log("raster retained redraw opportunity " +
          std::to_string(opportunity ? opportunity->id : 0) + " views " +
          Join(views));
      if (!opportunity.has_value()) {
        return;
      }
      coverage_.retained_redraws++;
      for (int64_t view : views) {
        if (painted_.count(view) > 0) {
          Present(*opportunity, view);
        } else {
          registry_.Complete(opportunity->id, opportunity->display_id, view,
                             FrameOpportunityOutcome::kNoVisualChange);
        }
      }
    });
  }

  std::mt19937 rng_;
  Options options_;
  FrameOpportunityRegistry registry_;
  fml::RefPtr<SteppedTaskRunner> ui_;
  fml::RefPtr<SteppedTaskRunner> raster_;
  fml::RefPtr<SteppedTaskRunner> platform_;
  fml::RefPtr<SteppedTaskRunner> io_;
  VsyncWaiterEmbedder* waiter_ = nullptr;
  std::unique_ptr<Animator> animator_;
  std::unique_ptr<FrameItem> resubmit_;

  std::map<int64_t, intptr_t> batons_;
  std::map<FrameOpportunityId, OpenOpportunity> open_;
  FrameOpportunityId next_opportunity_ = 1000;
  uint64_t configure_serial_ = 0;

  std::set<int64_t> metrics_views_;
  std::set<int64_t> dirty_;
  std::set<int64_t> rendered_during_frame_;
  std::set<int64_t> painted_;
  std::set<int64_t> hidden_;
  std::set<int64_t> removed_;
  bool ticks_enabled_ = true;
  bool display_present_ = true;
  Coverage coverage_;
  std::vector<std::string> log_;
};

constexpr uint32_t kSeeds = 2000;

Coverage RunSeeds(const Options& options, const char* label) {
  Coverage coverage;
  const uint32_t first = options.first_seed;
  for (uint32_t seed = first; seed < first + kSeeds; seed++) {
    Harness harness(seed, options);
    for (int step = 0; step < options.steps; step++) {
      harness.Step();
    }
    harness.Quiesce();
    coverage.Add(harness.coverage());
    const auto stranded = harness.StrandedDisplayTargets();
    if (!stranded.empty()) {
      std::ostringstream owed;
      for (const auto& [id, targets] : stranded) {
        owed << " opportunity " << id << " owes";
        for (int64_t target : targets) {
          owed << " " << target;
        }
      }
      ADD_FAILURE() << label << ": seed " << seed
                    << " strands display targets:" << owed.str() << "\n"
                    << harness.Trace();
      return coverage;
    }
    if (options.display_churn) {
      const auto unbuilt = harness.UnbuiltDirtyViews();
      if (!unbuilt.empty()) {
        std::ostringstream views;
        for (int64_t view : unbuilt) {
          views << " " << view;
        }
        ADD_FAILURE() << label << ": seed " << seed
                      << " never builds dirty visible views:" << views.str()
                      << "\n"
                      << harness.Trace();
        return coverage;
      }
    }
  }
  std::cout << label << ": " << kSeeds << " seeds, every display target "
            << "terminal\n  " << coverage.Describe() << std::endl;
  return coverage;
}

TEST(AvioFrameConservation, GlobalLaneLentDisplayTargets) {
  const Coverage coverage =
      RunSeeds(Options{}, "global lane lent the display's targets");
  // The harness sees the strand a host makes when it lends display targets to
  // the global lane, which never renders them.
  EXPECT_GT(coverage.stranded_global, 0u);
}

TEST(AvioFrameConservation, TargetlessGlobalLane) {
  Options options;
  options.global_lane_lent_display_targets = false;
  const Coverage coverage = RunSeeds(options, "targetless global lane");
  EXPECT_EQ(coverage.stranded_global, 0u);
}

TEST(AvioFrameConservation, ImmediateConfigureFrames) {
  Options options;
  options.immediate_frames = true;
  RunSeeds(options, "immediate configure frames");
}

TEST(AvioFrameConservation, ViewTopologyAndCancellation) {
  Options options;
  options.topology = true;
  options.immediate_frames = true;
  RunSeeds(options, "view and display topology with cancellation");
}

TEST(AvioFrameConservation, TargetlessGlobalLaneWithViewTopology) {
  Options options;
  options.topology = true;
  options.global_lane_lent_display_targets = false;
  RunSeeds(options, "targetless global lane with view topology");
}

TEST(AvioFrameConservation, DisplayLeavesAndReturnsWhileFramesAreOwed) {
  Options options;
  options.topology = true;
  options.display_churn = true;
  options.immediate_frames = true;
  options.global_lane_lent_display_targets = false;
  const Coverage coverage =
      RunSeeds(options, "display leaves and returns while frames are owed");
  EXPECT_GT(coverage.display_returns, 0u);
}

}  // namespace
}  // namespace testing
}  // namespace flutter
