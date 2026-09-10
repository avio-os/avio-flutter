// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/fl_view_visibility_monitor.h"

#include <utility>
#include <vector>

#include "flutter/shell/platform/embedder/test_utils/proc_table_replacement.h"
#include "flutter/shell/platform/linux/testing/linux_test.h"
#include "flutter/shell/platform/linux/testing/mock_gtk.h"
#include "gtest/gtest.h"

// Deliver map/unmap signals without creating or mapping a native test window.
// GtkWindow's real class handlers require an actual realized GdkWindow.
struct VisibilityTestWindow {
  GtkWindow parent_instance;
};
struct VisibilityTestWindowClass {
  GtkWindowClass parent_class;
};
G_DEFINE_TYPE(VisibilityTestWindow, visibility_test_window, GTK_TYPE_WINDOW);
static void visibility_test_window_class_init(
    VisibilityTestWindowClass* klass) {
  GTK_WIDGET_CLASS(klass)->map = [](GtkWidget*) {};
  GTK_WIDGET_CLASS(klass)->unmap = [](GtkWidget*) {};
}
static void visibility_test_window_init(VisibilityTestWindow*) {}
static GtkWidget* test_window_new() {
  return GTK_WIDGET(g_object_new(visibility_test_window_get_type(), nullptr));
}

// NOLINTBEGIN(clang-analyzer-core.StackAddressEscape)
class FlViewVisibilityMonitorTest : public flutter::testing::LinuxTest {
 protected:
  void SetUp() override {
    StartEngine();
    fl_engine_get_embedder_api(engine)->SetAvioViewVisibility =
        MOCK_ENGINE_PROC(
            SetAvioViewVisibility,
            ([this](auto, const FlutterAvioViewVisibilityEvent* event) {
              EXPECT_EQ(event->struct_size, sizeof(*event));
              events.emplace_back(event->view_id, event->visibility);
              return kSuccess;
            }));
  }

  void Visibility(GtkWidget* window, GdkVisibilityState state) {
    GdkEvent event = {};
    event.visibility.type = GDK_VISIBILITY_NOTIFY;
    event.visibility.state = state;
    gboolean handled = TRUE;
    g_signal_emit_by_name(window, "visibility-notify-event", &event, &handled);
    EXPECT_FALSE(handled);
  }

  void WindowState(GtkWidget* window, GdkWindowState state) {
    GdkEvent event = {};
    event.window_state.type = GDK_WINDOW_STATE;
    event.window_state.new_window_state = state;
    gboolean handled = TRUE;
    g_signal_emit_by_name(window, "window-state-event", &event, &handled);
    EXPECT_FALSE(handled);
  }

  ::testing::NiceMock<flutter::testing::MockGtk> mock_gtk;
  std::vector<std::pair<FlutterViewId, FlutterAvioViewVisibility>> events;
};

TEST_F(FlViewVisibilityMonitorTest, MapSuspendRestoreAndFocusAreIndependent) {
  g_autoptr(GtkWidget) window = test_window_new();
  g_object_ref_sink(window);
  g_autoptr(FlViewVisibilityMonitor) monitor =
      fl_view_visibility_monitor_new(engine, 0, window, GTK_WINDOW(window));
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events.back().second, kFlutterAvioViewVisibilitySuspended);
  g_signal_emit_by_name(window, "map");
  EXPECT_EQ(events.back().second, kFlutterAvioViewVisibilityVisible);
  const size_t mapped_events = events.size();
  WindowState(window, GDK_WINDOW_STATE_FOCUSED);
  WindowState(window, static_cast<GdkWindowState>(0));
  Visibility(window, GDK_VISIBILITY_PARTIAL);
  EXPECT_EQ(events.size(), mapped_events);
  Visibility(window, GDK_VISIBILITY_FULLY_OBSCURED);
  EXPECT_EQ(events.back().second, kFlutterAvioViewVisibilityObscured);
  Visibility(window, GDK_VISIBILITY_FULLY_OBSCURED);
  EXPECT_EQ(events.size(), mapped_events + 1);
  Visibility(window, GDK_VISIBILITY_UNOBSCURED);
  EXPECT_EQ(events.back().second, kFlutterAvioViewVisibilityVisible);
  g_signal_emit_by_name(window, "unmap");
  EXPECT_EQ(events.back().second, kFlutterAvioViewVisibilitySuspended);
  Visibility(window, GDK_VISIBILITY_UNOBSCURED);
  EXPECT_EQ(events.back().second, kFlutterAvioViewVisibilitySuspended);
  g_signal_emit_by_name(window, "map");
  EXPECT_EQ(events.back().second, kFlutterAvioViewVisibilityVisible);
  WindowState(window, GDK_WINDOW_STATE_ICONIFIED);
  EXPECT_EQ(events.back().second, kFlutterAvioViewVisibilitySuspended);
  WindowState(window, static_cast<GdkWindowState>(0));
  EXPECT_EQ(events.back().second, kFlutterAvioViewVisibilityVisible);
}

TEST_F(FlViewVisibilityMonitorTest, ViewsHaveIndependentRelevanceAndTeardown) {
  g_autoptr(GtkWidget) first = test_window_new();
  g_object_ref_sink(first);
  g_autoptr(GtkWidget) second = test_window_new();
  g_object_ref_sink(second);
  g_autoptr(FlViewVisibilityMonitor) first_monitor =
      fl_view_visibility_monitor_new(engine, 0, first, GTK_WINDOW(first));
  g_autoptr(FlViewVisibilityMonitor) second_monitor =
      fl_view_visibility_monitor_new(engine, 42, second, GTK_WINDOW(second));
  g_signal_emit_by_name(first, "map");
  g_signal_emit_by_name(second, "map");
  EXPECT_EQ(events.back().first, 42);
  EXPECT_EQ(events.back().second, kFlutterAvioViewVisibilityVisible);
  Visibility(first, GDK_VISIBILITY_FULLY_OBSCURED);
  EXPECT_EQ(events.back().first, 0);
  EXPECT_EQ(events.back().second, kFlutterAvioViewVisibilityObscured);
  g_clear_object(&first_monitor);
  EXPECT_EQ(events.back().first, 0);
  EXPECT_EQ(events.back().second, kFlutterAvioViewVisibilitySuspended);
  const size_t before_dispose = events.size();
  Visibility(first, GDK_VISIBILITY_UNOBSCURED);
  g_signal_emit_by_name(first, "unmap");
  EXPECT_EQ(events.size(), before_dispose);
  Visibility(second, GDK_VISIBILITY_FULLY_OBSCURED);
  EXPECT_EQ(events.back().first, 42);
  EXPECT_EQ(events.back().second, kFlutterAvioViewVisibilityObscured);
}
TEST_F(FlViewVisibilityMonitorTest, LateSiblingInheritsWindowSuspension) {
  g_autoptr(GtkWidget) window = test_window_new();
  g_object_ref_sink(window);
  g_autoptr(FlViewVisibilityMonitor) first =
      fl_view_visibility_monitor_new(engine, 0, window, GTK_WINDOW(window));
  g_signal_emit_by_name(window, "map");
  Visibility(window, GDK_VISIBILITY_FULLY_OBSCURED);
  g_clear_object(&first);
  // The toolkit will not repeat this transition for a newly attached sibling.
  g_autoptr(FlViewVisibilityMonitor) later =
      fl_view_visibility_monitor_new(engine, 42, window, GTK_WINDOW(window));
  g_signal_emit_by_name(window, "map");
  EXPECT_EQ(events.back().first, 42);
  EXPECT_EQ(events.back().second, kFlutterAvioViewVisibilityObscured);
  Visibility(window, GDK_VISIBILITY_UNOBSCURED);
  EXPECT_EQ(events.back().second, kFlutterAvioViewVisibilityVisible);
}
// NOLINTEND(clang-analyzer-core.StackAddressEscape)
