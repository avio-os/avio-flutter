// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_FL_ATSPI_MONITOR_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_FL_ATSPI_MONITOR_H_

#include <glib-object.h>

#include "flutter/shell/platform/linux/fl_semantics_demand.h"

G_BEGIN_DECLS

G_DECLARE_FINAL_TYPE(FlAtspiMonitor,
                     fl_atspi_monitor,
                     FL,
                     ATSPI_MONITOR,
                     GObject);

/**
 * FlAtspiMonitor:
 *
 * Reports the standing AT-SPI sources of semantics demand to an
 * #FlSemanticsDemand: `org.a11y.Status`'s `ScreenReaderEnabled` on the
 * session bus, and whether the registry lists an event listener GTK's ATK
 * bridge emits to. The registry answers `GetRegisteredEvents` with the
 * listeners for every application and those scoped to the asking
 * connection, so the question is asked on the bridge's own connection
 * (libatspi's shared one); the registry's change signals reach every
 * connection and only prompt the question again. Signals are never trusted
 * for state. Where nothing hosts accessibility (no launcher on the session
 * bus, an accessibility bus that refuses connections, a bridge without a
 * connection) no assistive client can read this application, so there is no
 * demand; a source that cannot otherwise be established wants semantics.
 */

/**
 * fl_atspi_monitor_new:
 * @demand: the #FlSemanticsDemand to report to.
 *
 * Returns: a new #FlAtspiMonitor. It watches nothing until started.
 */
FlAtspiMonitor* fl_atspi_monitor_new(FlSemanticsDemand* demand);

/**
 * fl_atspi_monitor_start:
 * @monitor: an #FlAtspiMonitor.
 *
 * Starts watching, asynchronously on the thread-default main context. With
 * `NO_AT_BRIDGE=1` GTK publishes no accessibility, so nothing is watched.
 */
void fl_atspi_monitor_start(FlAtspiMonitor* monitor);

/**
 * fl_atspi_monitor_serving_request:
 * @monitor: an #FlAtspiMonitor.
 *
 * Whether the bridge's connection is dispatching an AT-SPI client's request
 * to this application now. GTK's bridge also reads its accessibles when it
 * builds its own cache, which it does in every application whenever a
 * listener appears on the bus; those reads are not demand.
 *
 * Returns: %TRUE while a request is dispatched, and until the monitor sees
 * the bridge's requests.
 */
gboolean fl_atspi_monitor_serving_request(FlAtspiMonitor* monitor);

G_END_DECLS

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_FL_ATSPI_MONITOR_H_
