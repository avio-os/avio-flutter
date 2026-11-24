// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/fl_semantics_demand.h"

struct _FlSemanticsDemand {
  GObject parent_instance;

  // How long semantics stay enabled after demand ends, in microseconds.
  gint64 hold_us;

  // Standing sources that want semantics (FlSemanticsDemandSource bits).
  guint sources;

  // Monotonic time of the last read, or of the end of the last standing
  // source, in microseconds.
  gint64 last_demand_us;

  gboolean enabled;

  // Fires when the hold may have expired; re-arms itself while it has not.
  guint expiry_source_id;
};

enum { PROP_0, PROP_ENABLED, PROP_LAST };

static GParamSpec* properties[PROP_LAST];

G_DEFINE_TYPE(FlSemanticsDemand, fl_semantics_demand, G_TYPE_OBJECT)

static void set_enabled(FlSemanticsDemand* self, gboolean enabled) {
  if (self->enabled == enabled) {
    return;
  }
  self->enabled = enabled;
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_ENABLED]);
}

static void arm_expiry(FlSemanticsDemand* self, gint64 delay_us);

static gboolean expiry_cb(gpointer user_data) {
  FlSemanticsDemand* self = FL_SEMANTICS_DEMAND(user_data);
  self->expiry_source_id = 0;
  if (self->sources != 0) {
    return G_SOURCE_REMOVE;
  }
  gint64 remaining_us =
      self->last_demand_us + self->hold_us - g_get_monotonic_time();
  if (remaining_us > 0) {
    arm_expiry(self, remaining_us);
  } else {
    set_enabled(self, FALSE);
  }
  return G_SOURCE_REMOVE;
}

// Arms one timer for the hold. A read only moves the deadline later, so an
// armed timer never fires too late; it re-arms for whatever remains.
static void arm_expiry(FlSemanticsDemand* self, gint64 delay_us) {
  if (self->expiry_source_id != 0) {
    return;
  }
  guint delay_ms = static_cast<guint>((MAX(delay_us, 0) + 999) / 1000);
  self->expiry_source_id = g_timeout_add(delay_ms, expiry_cb, self);
}

static void fl_semantics_demand_get_property(GObject* object,
                                             guint prop_id,
                                             GValue* value,
                                             GParamSpec* pspec) {
  FlSemanticsDemand* self = FL_SEMANTICS_DEMAND(object);
  switch (prop_id) {
    case PROP_ENABLED:
      g_value_set_boolean(value, self->enabled);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void fl_semantics_demand_dispose(GObject* object) {
  FlSemanticsDemand* self = FL_SEMANTICS_DEMAND(object);
  if (self->expiry_source_id != 0) {
    g_source_remove(self->expiry_source_id);
    self->expiry_source_id = 0;
  }
  G_OBJECT_CLASS(fl_semantics_demand_parent_class)->dispose(object);
}

static void fl_semantics_demand_class_init(FlSemanticsDemandClass* klass) {
  G_OBJECT_CLASS(klass)->get_property = fl_semantics_demand_get_property;
  G_OBJECT_CLASS(klass)->dispose = fl_semantics_demand_dispose;
  properties[PROP_ENABLED] = g_param_spec_boolean(
      "enabled", "enabled", "Whether the engine builds semantics", FALSE,
      static_cast<GParamFlags>(G_PARAM_READABLE | G_PARAM_STATIC_STRINGS |
                               G_PARAM_EXPLICIT_NOTIFY));
  g_object_class_install_properties(G_OBJECT_CLASS(klass), PROP_LAST,
                                    properties);
}

static void fl_semantics_demand_init(FlSemanticsDemand* self) {}

FlSemanticsDemand* fl_semantics_demand_new(guint hold_ms) {
  FlSemanticsDemand* self = FL_SEMANTICS_DEMAND(
      g_object_new(fl_semantics_demand_get_type(), nullptr));
  self->hold_us = static_cast<gint64>(hold_ms) * 1000;
  return self;
}

void fl_semantics_demand_set_hold(FlSemanticsDemand* self, guint hold_ms) {
  g_return_if_fail(FL_IS_SEMANTICS_DEMAND(self));
  self->hold_us = static_cast<gint64>(hold_ms) * 1000;
}

void fl_semantics_demand_set_source(FlSemanticsDemand* self,
                                    FlSemanticsDemandSource source,
                                    gboolean wanted) {
  g_return_if_fail(FL_IS_SEMANTICS_DEMAND(self));
  guint before = self->sources;
  if (wanted) {
    self->sources |= source;
  } else {
    self->sources &= ~static_cast<guint>(source);
  }
  if (self->sources != 0) {
    set_enabled(self, TRUE);
  } else if (before != 0) {
    self->last_demand_us = g_get_monotonic_time();
    if (self->enabled) {
      arm_expiry(self, self->hold_us);
    }
  }
}

void fl_semantics_demand_note_read(FlSemanticsDemand* self) {
  g_return_if_fail(FL_IS_SEMANTICS_DEMAND(self));
  self->last_demand_us = g_get_monotonic_time();
  if (self->sources == 0) {
    arm_expiry(self, self->hold_us);
  }
  set_enabled(self, TRUE);
}

gboolean fl_semantics_demand_get_enabled(FlSemanticsDemand* self) {
  g_return_val_if_fail(FL_IS_SEMANTICS_DEMAND(self), FALSE);
  return self->enabled;
}
