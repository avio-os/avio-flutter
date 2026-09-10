// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/fl_view_visibility_monitor.h"

// One observer owns the toolkit window's transition-only visibility state.
// Keep it on the GtkWindow so a later FlView inherits an existing suspension.
G_DECLARE_FINAL_TYPE(FlWindowVisibility,
                     fl_window_visibility,
                     FL,
                     WINDOW_VISIBILITY,
                     GObject);
struct _FlWindowVisibility {
  GObject parent_instance;
  gboolean hidden;
  gboolean obscured;
};
G_DEFINE_TYPE(FlWindowVisibility, fl_window_visibility, G_TYPE_OBJECT);
static guint window_visibility_changed;

static gboolean is_hidden(GdkWindowState state) {
  return (state & (GDK_WINDOW_STATE_WITHDRAWN | GDK_WINDOW_STATE_ICONIFIED)) !=
         0;
}

static gboolean window_state_cb(FlWindowVisibility* self,
                                GdkEventWindowState* event) {
  self->hidden = is_hidden(event->new_window_state);
  g_signal_emit(self, window_visibility_changed, 0);
  return FALSE;
}

static gboolean visibility_cb(FlWindowVisibility* self,
                              GdkEventVisibility* event) {
  // GTK maps Wayland xdg_toplevel suspended to FULLY_OBSCURED, not ICONIFIED.
  // PARTIAL visibility is still renderable; focus is never a visibility input.
  self->obscured = event->state == GDK_VISIBILITY_FULLY_OBSCURED;
  g_signal_emit(self, window_visibility_changed, 0);
  return FALSE;
}

static void fl_window_visibility_class_init(FlWindowVisibilityClass* klass) {
  window_visibility_changed =
      g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                   nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
}
static void fl_window_visibility_init(FlWindowVisibility* self) {}

static FlWindowVisibility* get_window_visibility(GtkWindow* window) {
  constexpr const char* key = "flutter-window-render-relevance";
  auto* state = static_cast<FlWindowVisibility*>(
      g_object_get_data(G_OBJECT(window), key));
  if (state == nullptr) {
    state = FL_WINDOW_VISIBILITY(
        g_object_new(fl_window_visibility_get_type(), nullptr));
    state->hidden = is_hidden(
        gdk_window_get_state(gtk_widget_get_window(GTK_WIDGET(window))));
    gtk_widget_add_events(GTK_WIDGET(window), GDK_VISIBILITY_NOTIFY_MASK);
    g_signal_connect_object(window, "window-state-event",
                            G_CALLBACK(window_state_cb), state,
                            G_CONNECT_SWAPPED);
    g_signal_connect_object(window, "visibility-notify-event",
                            G_CALLBACK(visibility_cb), state,
                            G_CONNECT_SWAPPED);
    g_object_set_data_full(G_OBJECT(window), key, state, g_object_unref);
  }
  return state;
}

struct _FlViewVisibilityMonitor {
  GObject parent_instance;
  FlEngine* engine;
  FlutterViewId view_id;
  FlWindowVisibility* window;
  gboolean mapped;
  gboolean published;
  FlutterAvioViewVisibility visibility;
};

G_DEFINE_TYPE(FlViewVisibilityMonitor,
              fl_view_visibility_monitor,
              G_TYPE_OBJECT);

static void publish(FlViewVisibilityMonitor* self) {
  const FlutterAvioViewVisibility visibility =
      !self->mapped || self->window->hidden
          ? kFlutterAvioViewVisibilitySuspended
      : self->window->obscured ? kFlutterAvioViewVisibilityObscured
                               : kFlutterAvioViewVisibilityVisible;
  if (!self->published || self->visibility != visibility) {
    self->published = TRUE;
    self->visibility = visibility;
    fl_engine_set_view_visibility(self->engine, self->view_id, visibility);
  }
}

static void map_cb(FlViewVisibilityMonitor* self) {
  self->mapped = TRUE;
  publish(self);
}

static void unmap_cb(FlViewVisibilityMonitor* self) {
  self->mapped = FALSE;
  publish(self);
}

static void fl_view_visibility_monitor_dispose(GObject* object) {
  auto* self = FL_VIEW_VISIBILITY_MONITOR(object);
  if (self->engine != nullptr) {
    // Implicit view 0 cannot be removed through the embedder API. A surviving
    // sibling must not keep its disposed predecessor renderable forever.
    self->mapped = FALSE;
    publish(self);
  }
  if (self->window != nullptr) {
    g_signal_handlers_disconnect_by_data(self->window, self);
  }
  g_clear_object(&self->window);
  g_clear_object(&self->engine);
  G_OBJECT_CLASS(fl_view_visibility_monitor_parent_class)->dispose(object);
}

static void fl_view_visibility_monitor_class_init(
    FlViewVisibilityMonitorClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = fl_view_visibility_monitor_dispose;
}

static void fl_view_visibility_monitor_init(FlViewVisibilityMonitor* self) {}

FlViewVisibilityMonitor* fl_view_visibility_monitor_new(FlEngine* engine,
                                                        FlutterViewId view_id,
                                                        GtkWidget* view,
                                                        GtkWindow* window) {
  g_return_val_if_fail(FL_IS_ENGINE(engine), nullptr);
  g_return_val_if_fail(GTK_IS_WIDGET(view), nullptr);
  g_return_val_if_fail(GTK_IS_WINDOW(window), nullptr);
  auto* self = FL_VIEW_VISIBILITY_MONITOR(
      g_object_new(fl_view_visibility_monitor_get_type(), nullptr));
  self->engine = FL_ENGINE(g_object_ref(engine));
  self->view_id = view_id;
  self->mapped = gtk_widget_get_mapped(view);
  self->window =
      FL_WINDOW_VISIBILITY(g_object_ref(get_window_visibility(window)));
  g_signal_connect_object(view, "map", G_CALLBACK(map_cb), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(view, "unmap", G_CALLBACK(unmap_cb), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(self->window, "changed", G_CALLBACK(publish), self,
                          G_CONNECT_SWAPPED);
  publish(self);
  return self;
}
