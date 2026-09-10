// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_FL_VIEW_VISIBILITY_MONITOR_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_FL_VIEW_VISIBILITY_MONITOR_H_

#include <gtk/gtk.h>

#include "flutter/shell/platform/linux/fl_engine_private.h"
#include "flutter/shell/platform/linux/fl_view_renderer.h"

G_BEGIN_DECLS

G_DECLARE_FINAL_TYPE(FlViewVisibilityMonitor,
                     fl_view_visibility_monitor,
                     FL,
                     VIEW_VISIBILITY_MONITOR,
                     GObject);

// Call after engine startup and view registration, while the widget is
// realized. Toolkit render relevance is per view and independent of application
// lifecycle and input focus. A cold renderer may produce its first drawable
// frame while unmapped so a first-frame handler can show its window. Its actual
// renderer receipt then restores normal mapped/window relevance, even if
// hidden. Destroy the monitor before removing the view from the engine.
FlViewVisibilityMonitor* fl_view_visibility_monitor_new(
    FlEngine* engine,
    FlutterViewId view_id,
    GtkWidget* view,
    GtkWindow* window,
    FlViewRenderer* renderer);

G_END_DECLS

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_FL_VIEW_VISIBILITY_MONITOR_H_
