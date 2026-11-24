// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_FL_SEMANTICS_DEMAND_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_FL_SEMANTICS_DEMAND_H_

#include <glib-object.h>

G_BEGIN_DECLS

G_DECLARE_FINAL_TYPE(FlSemanticsDemand,
                     fl_semantics_demand,
                     FL,
                     SEMANTICS_DEMAND,
                     GObject);

/**
 * FlSemanticsDemandSource:
 * @FL_SEMANTICS_DEMAND_SOURCE_SCREEN_READER: `org.a11y.Status`'s
 *   `ScreenReaderEnabled`.
 * @FL_SEMANTICS_DEMAND_SOURCE_LISTENERS: the AT-SPI registry lists an event
 *   listener for every application or for this one.
 *
 * The standing sources of semantics demand. A source that cannot be
 * established is reported as wanting semantics.
 */
typedef enum {
  FL_SEMANTICS_DEMAND_SOURCE_SCREEN_READER = 1 << 0,
  FL_SEMANTICS_DEMAND_SOURCE_LISTENERS = 1 << 1,
} FlSemanticsDemandSource;

/**
 * FlSemanticsDemand:
 *
 * The one authority deciding whether the engine builds its semantics tree.
 * Semantics are wanted while a standing source wants them, and for a hold
 * after the last read of a view's accessible or the end of the last standing
 * source. The #FlSemanticsDemand:enabled property carries the decision; the
 * engine and its views follow it.
 */

/**
 * fl_semantics_demand_new:
 * @hold_ms: how long semantics stay enabled after demand ends.
 *
 * Returns: a new #FlSemanticsDemand, disabled.
 */
FlSemanticsDemand* fl_semantics_demand_new(guint hold_ms);

/**
 * fl_semantics_demand_set_hold:
 * @demand: an #FlSemanticsDemand.
 * @hold_ms: how long semantics stay enabled after demand ends.
 *
 * Changes how long semantics stay enabled after demand ends. Set it before
 * demand ends: a hold already being timed keeps its timer.
 */
void fl_semantics_demand_set_hold(FlSemanticsDemand* demand, guint hold_ms);

/**
 * fl_semantics_demand_set_source:
 * @demand: an #FlSemanticsDemand.
 * @source: the source that changed.
 * @wanted: whether @source wants semantics.
 *
 * Records the state of a standing source of demand.
 */
void fl_semantics_demand_set_source(FlSemanticsDemand* demand,
                                    FlSemanticsDemandSource source,
                                    gboolean wanted);

/**
 * fl_semantics_demand_note_read:
 * @demand: an #FlSemanticsDemand.
 *
 * Records that an assistive client read a view's accessible. Enables
 * semantics at once and restarts the hold.
 */
void fl_semantics_demand_note_read(FlSemanticsDemand* demand);

/**
 * fl_semantics_demand_get_enabled:
 * @demand: an #FlSemanticsDemand.
 *
 * Returns: %TRUE while the engine should build its semantics tree.
 */
gboolean fl_semantics_demand_get_enabled(FlSemanticsDemand* demand);

G_END_DECLS

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_FL_SEMANTICS_DEMAND_H_
