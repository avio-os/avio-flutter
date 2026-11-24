// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/fl_atspi_monitor.h"

#include <atspi/atspi.h>
#include <dbus/dbus.h>
#include <gio/gio.h>

#include <cstdlib>
#include <cstring>

static constexpr char kBusName[] = "org.a11y.Bus";
static constexpr char kBusPath[] = "/org/a11y/bus";
static constexpr char kBusInterface[] = "org.a11y.Bus";
static constexpr char kStatusInterface[] = "org.a11y.Status";
static constexpr char kScreenReaderProperty[] = "ScreenReaderEnabled";
static constexpr char kPropertiesInterface[] =
    "org.freedesktop.DBus.Properties";
static constexpr char kRegistryName[] = "org.a11y.atspi.Registry";
static constexpr char kRegistryPath[] = "/org/a11y/atspi/registry";
static constexpr char kRegistryInterface[] = "org.a11y.atspi.Registry";
static constexpr int kQueryTimeoutMs = 1000;

struct _FlAtspiMonitor {
  GObject parent_instance;

  FlSemanticsDemand* demand;

  GCancellable* cancellable;

  // The session bus, for org.a11y.Status.
  GDBusConnection* session;
  guint status_subscription;

  // This monitor's own connection to the accessibility bus. It only hears
  // the registry's change signals, which GTK's bridge consumes on its own.
  GDBusConnection* a11y;
  guint registry_subscription;
  guint registry_owner_subscription;

  // The connection GTK's ATK bridge publishes this application on.
  DBusConnection* bridge;

  // The GetRegisteredEvents call in flight, and whether a change arrived
  // while it was.
  DBusPendingCall* query;
  gboolean query_again;

  // Whether this monitor sees the bridge's incoming requests, and whether
  // one is being dispatched now. The bridge also walks its own tree when a
  // listener appears anywhere on the bus, so only reads made for a request
  // are demand.
  gboolean filtering;
  gboolean serving_request;
  guint request_end_source;
};

G_DEFINE_TYPE(FlAtspiMonitor, fl_atspi_monitor, G_TYPE_OBJECT)

// Callbacks hold the monitor weakly: GDBus may still deliver a subscribed
// signal or a cancelled reply after the monitor is gone.
static GWeakRef* weak_box_new(FlAtspiMonitor* self) {
  GWeakRef* box = g_new(GWeakRef, 1);
  g_weak_ref_init(box, self);
  return box;
}

static void weak_box_free(gpointer data) {
  GWeakRef* box = static_cast<GWeakRef*>(data);
  g_weak_ref_clear(box);
  g_free(box);
}

static FlAtspiMonitor* weak_box_get(gpointer data) {
  return static_cast<FlAtspiMonitor*>(
      g_weak_ref_get(static_cast<GWeakRef*>(data)));
}

// Takes and frees a one-shot callback's box.
static FlAtspiMonitor* weak_box_take(gpointer data) {
  FlAtspiMonitor* self = weak_box_get(data);
  weak_box_free(data);
  return self;
}

// A source that cannot be established wants semantics, as upstream's
// always-on tree did.
static void unavailable(FlAtspiMonitor* self,
                        FlSemanticsDemandSource source,
                        const gchar* reason) {
  g_debug("Semantics demand source unavailable, keeping semantics: %s", reason);
  fl_semantics_demand_set_source(self->demand, source, TRUE);
}

// Nothing hosts accessibility, so GTK's bridge publishes nowhere and no
// assistive client can read this application.
static void absent(FlAtspiMonitor* self,
                   FlSemanticsDemandSource source,
                   const gchar* reason) {
  g_debug("No accessibility bus, so no semantics demand: %s", reason);
  fl_semantics_demand_set_source(self->demand, source, FALSE);
}

// Whether |error| says the session runs no accessibility bus launcher.
static gboolean no_launcher(const GError* error) {
  return g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
         g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER);
}

static void screen_reader_cb(GObject* object,
                             GAsyncResult* result,
                             gpointer user_data) {
  g_autoptr(FlAtspiMonitor) self = weak_box_take(user_data);
  g_autoptr(GError) error = nullptr;
  g_autoptr(GVariant) reply =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &error);
  if (self == nullptr ||
      g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    return;
  }
  if (no_launcher(error)) {
    absent(self, FL_SEMANTICS_DEMAND_SOURCE_SCREEN_READER, error->message);
    return;
  }
  g_autoptr(GVariant) value = nullptr;
  if (reply != nullptr) {
    g_variant_get(reply, "(v)", &value);
  }
  if (value == nullptr ||
      !g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
    unavailable(self, FL_SEMANTICS_DEMAND_SOURCE_SCREEN_READER,
                error != nullptr ? error->message : "not a boolean");
    return;
  }
  fl_semantics_demand_set_source(self->demand,
                                 FL_SEMANTICS_DEMAND_SOURCE_SCREEN_READER,
                                 g_variant_get_boolean(value));
}

// The bridge would have started the launcher; this never does.
static void read_screen_reader(FlAtspiMonitor* self) {
  g_dbus_connection_call(
      self->session, kBusName, kBusPath, kPropertiesInterface, "Get",
      g_variant_new("(ss)", kStatusInterface, kScreenReaderProperty),
      G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, -1,
      self->cancellable, screen_reader_cb, weak_box_new(self));
}

static void status_changed_cb(GDBusConnection* connection,
                              const gchar* sender_name,
                              const gchar* object_path,
                              const gchar* interface_name,
                              const gchar* signal_name,
                              GVariant* parameters,
                              gpointer user_data) {
  g_autoptr(FlAtspiMonitor) self = weak_box_get(user_data);
  if (self != nullptr) {
    read_screen_reader(self);
  }
}

static gboolean request_end_cb(gpointer user_data) {
  FlAtspiMonitor* self = FL_ATSPI_MONITOR(user_data);
  self->serving_request = FALSE;
  self->request_end_source = 0;
  return G_SOURCE_REMOVE;
}

// Marks an AT-SPI client's request to this application. libatspi dispatches
// one message per main-loop dispatch, so a high-priority idle ends the mark
// before the next message, such as a registry signal that makes the bridge
// walk its tree.
static DBusHandlerResult request_filter(DBusConnection* connection,
                                        DBusMessage* message,
                                        void* user_data) {
  FlAtspiMonitor* self = FL_ATSPI_MONITOR(user_data);
  if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_METHOD_CALL) {
    self->serving_request = TRUE;
    if (self->request_end_source == 0) {
      self->request_end_source =
          g_idle_add_full(G_PRIORITY_HIGH, request_end_cb, self, nullptr);
    }
  }
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void listeners_cb(DBusPendingCall* call, void* user_data);

// Asks the registry, on the bridge's connection, for the listeners this
// application emits to. One question is in flight at a time; a change that
// arrives meanwhile asks again once it is answered.
static void query_listeners(FlAtspiMonitor* self) {
  if (self->bridge == nullptr) {
    return;
  }
  if (self->query != nullptr) {
    self->query_again = TRUE;
    return;
  }
  DBusMessage* message = dbus_message_new_method_call(
      kRegistryName, kRegistryPath, kRegistryInterface, "GetRegisteredEvents");
  if (message == nullptr) {
    unavailable(self, FL_SEMANTICS_DEMAND_SOURCE_LISTENERS, "out of memory");
    return;
  }
  dbus_bool_t sent = dbus_connection_send_with_reply(
      self->bridge, message, &self->query, kQueryTimeoutMs);
  dbus_message_unref(message);
  if (!sent || self->query == nullptr) {
    self->query = nullptr;
    unavailable(self, FL_SEMANTICS_DEMAND_SOURCE_LISTENERS,
                "the bridge's connection is closed");
    return;
  }
  // Dispose cancels the call before the monitor is freed, so the notify
  // never outlives it.
  dbus_pending_call_set_notify(self->query, listeners_cb, self, nullptr);
}

static void listeners_cb(DBusPendingCall* call, void* user_data) {
  FlAtspiMonitor* self = FL_ATSPI_MONITOR(user_data);
  DBusMessage* reply = dbus_pending_call_steal_reply(call);
  dbus_pending_call_unref(self->query);
  self->query = nullptr;

  gboolean parsed = FALSE;
  gboolean listening = FALSE;
  if (reply != nullptr &&
      dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN &&
      dbus_message_has_signature(reply, "a(ss)")) {
    DBusMessageIter iter, listeners;
    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_recurse(&iter, &listeners);
    listening = dbus_message_iter_get_arg_type(&listeners) == DBUS_TYPE_STRUCT;
    parsed = TRUE;
  }
  if (reply != nullptr) {
    dbus_message_unref(reply);
  }

  if (self->query_again) {
    // A newer answer is coming; it supersedes this one.
    self->query_again = FALSE;
    query_listeners(self);
  } else if (!parsed) {
    unavailable(self, FL_SEMANTICS_DEMAND_SOURCE_LISTENERS,
                "GetRegisteredEvents failed");
  } else {
    fl_semantics_demand_set_source(
        self->demand, FL_SEMANTICS_DEMAND_SOURCE_LISTENERS, listening);
  }
}

static void registry_changed_cb(GDBusConnection* connection,
                                const gchar* sender_name,
                                const gchar* object_path,
                                const gchar* interface_name,
                                const gchar* signal_name,
                                GVariant* parameters,
                                gpointer user_data) {
  g_autoptr(FlAtspiMonitor) self = weak_box_get(user_data);
  if (self != nullptr) {
    query_listeners(self);
  }
}

static void a11y_ready_cb(GObject* object,
                          GAsyncResult* result,
                          gpointer user_data) {
  g_autoptr(FlAtspiMonitor) self = weak_box_take(user_data);
  g_autoptr(GError) error = nullptr;
  g_autoptr(GDBusConnection) a11y =
      g_dbus_connection_new_for_address_finish(result, &error);
  if (self == nullptr ||
      g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    return;
  }
  if (a11y == nullptr) {
    // The bridge connects to the same address, so it could not either.
    absent(self, FL_SEMANTICS_DEMAND_SOURCE_LISTENERS, error->message);
    return;
  }

  // The bridge publishes on libatspi's shared connection. Ask only when it
  // is on the bus this monitor hears.
  DBusConnection* bridge = atspi_get_a11y_bus();
  if (bridge == nullptr) {
    absent(self, FL_SEMANTICS_DEMAND_SOURCE_LISTENERS,
           "the ATK bridge has no connection");
    return;
  }
  char* bridge_bus = dbus_connection_get_server_id(bridge);
  const gchar* monitor_bus = g_dbus_connection_get_guid(a11y);
  gboolean same_bus = bridge_bus != nullptr && monitor_bus != nullptr &&
                      strcmp(bridge_bus, monitor_bus) == 0;
  dbus_free(bridge_bus);
  if (!same_bus) {
    unavailable(self, FL_SEMANTICS_DEMAND_SOURCE_LISTENERS,
                "the ATK bridge is on another bus");
    return;
  }

  self->a11y = G_DBUS_CONNECTION(g_steal_pointer(&a11y));
  self->bridge = dbus_connection_ref(bridge);
  self->filtering =
      dbus_connection_add_filter(self->bridge, request_filter, self, nullptr);
  self->registry_subscription = g_dbus_connection_signal_subscribe(
      self->a11y, nullptr, kRegistryInterface, nullptr, kRegistryPath, nullptr,
      G_DBUS_SIGNAL_FLAGS_NONE, registry_changed_cb, weak_box_new(self),
      weak_box_free);
  self->registry_owner_subscription = g_dbus_connection_signal_subscribe(
      self->a11y, nullptr, "org.freedesktop.DBus", "NameOwnerChanged",
      "/org/freedesktop/DBus", kRegistryName, G_DBUS_SIGNAL_FLAGS_NONE,
      registry_changed_cb, weak_box_new(self), weak_box_free);
  query_listeners(self);
}

static void connect_a11y(FlAtspiMonitor* self, const gchar* address) {
  g_dbus_connection_new_for_address(
      address,
      static_cast<GDBusConnectionFlags>(
          G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr, self->cancellable, a11y_ready_cb, weak_box_new(self));
}

static void address_cb(GObject* object,
                       GAsyncResult* result,
                       gpointer user_data) {
  g_autoptr(FlAtspiMonitor) self = weak_box_take(user_data);
  g_autoptr(GError) error = nullptr;
  g_autoptr(GVariant) reply =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &error);
  if (self == nullptr ||
      g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    return;
  }
  if (no_launcher(error)) {
    absent(self, FL_SEMANTICS_DEMAND_SOURCE_LISTENERS, error->message);
    return;
  }
  if (reply == nullptr) {
    unavailable(self, FL_SEMANTICS_DEMAND_SOURCE_LISTENERS, error->message);
    return;
  }
  const gchar* address = nullptr;
  g_variant_get(reply, "(&s)", &address);
  connect_a11y(self, address);
}

static void session_ready_cb(GObject* object,
                             GAsyncResult* result,
                             gpointer user_data) {
  g_autoptr(FlAtspiMonitor) self = weak_box_take(user_data);
  g_autoptr(GError) error = nullptr;
  g_autoptr(GDBusConnection) session = g_bus_get_finish(result, &error);
  if (self == nullptr ||
      g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    return;
  }
  // The same address libatspi, and so GTK's bridge, connects to first.
  const gchar* address = g_getenv("AT_SPI_BUS_ADDRESS");
  gboolean named = address != nullptr && address[0] != '\0';
  if (session == nullptr) {
    // No org.a11y.Status, and an accessibility bus only where one is named.
    absent(self, FL_SEMANTICS_DEMAND_SOURCE_SCREEN_READER, error->message);
    if (named) {
      connect_a11y(self, address);
    } else {
      absent(self, FL_SEMANTICS_DEMAND_SOURCE_LISTENERS, error->message);
    }
    return;
  }
  self->session = G_DBUS_CONNECTION(g_steal_pointer(&session));
  self->status_subscription = g_dbus_connection_signal_subscribe(
      self->session, nullptr, kPropertiesInterface, "PropertiesChanged",
      kBusPath, kStatusInterface, G_DBUS_SIGNAL_FLAGS_NONE, status_changed_cb,
      weak_box_new(self), weak_box_free);
  read_screen_reader(self);

  if (named) {
    connect_a11y(self, address);
    return;
  }
  g_dbus_connection_call(self->session, kBusName, kBusPath, kBusInterface,
                         "GetAddress", nullptr, G_VARIANT_TYPE("(s)"),
                         G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, self->cancellable,
                         address_cb, weak_box_new(self));
}

static void fl_atspi_monitor_dispose(GObject* object) {
  FlAtspiMonitor* self = FL_ATSPI_MONITOR(object);

  g_cancellable_cancel(self->cancellable);
  if (self->query != nullptr) {
    dbus_pending_call_cancel(self->query);
    dbus_pending_call_unref(self->query);
    self->query = nullptr;
  }
  if (self->bridge != nullptr) {
    if (self->filtering) {
      dbus_connection_remove_filter(self->bridge, request_filter, self);
      self->filtering = FALSE;
    }
    dbus_connection_unref(self->bridge);
    self->bridge = nullptr;
  }
  if (self->request_end_source != 0) {
    g_source_remove(self->request_end_source);
    self->request_end_source = 0;
  }
  if (self->session != nullptr && self->status_subscription != 0) {
    g_dbus_connection_signal_unsubscribe(self->session,
                                         self->status_subscription);
    self->status_subscription = 0;
  }
  if (self->a11y != nullptr) {
    if (self->registry_subscription != 0) {
      g_dbus_connection_signal_unsubscribe(self->a11y,
                                           self->registry_subscription);
      self->registry_subscription = 0;
    }
    if (self->registry_owner_subscription != 0) {
      g_dbus_connection_signal_unsubscribe(self->a11y,
                                           self->registry_owner_subscription);
      self->registry_owner_subscription = 0;
    }
    g_dbus_connection_close(self->a11y, nullptr, nullptr, nullptr);
  }
  g_clear_object(&self->a11y);
  g_clear_object(&self->session);
  g_clear_object(&self->cancellable);
  g_clear_object(&self->demand);

  G_OBJECT_CLASS(fl_atspi_monitor_parent_class)->dispose(object);
}

static void fl_atspi_monitor_class_init(FlAtspiMonitorClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = fl_atspi_monitor_dispose;
}

static void fl_atspi_monitor_init(FlAtspiMonitor* self) {
  self->cancellable = g_cancellable_new();
}

FlAtspiMonitor* fl_atspi_monitor_new(FlSemanticsDemand* demand) {
  g_return_val_if_fail(FL_IS_SEMANTICS_DEMAND(demand), nullptr);
  FlAtspiMonitor* self =
      FL_ATSPI_MONITOR(g_object_new(fl_atspi_monitor_get_type(), nullptr));
  self->demand = FL_SEMANTICS_DEMAND(g_object_ref(demand));
  return self;
}

gboolean fl_atspi_monitor_serving_request(FlAtspiMonitor* self) {
  g_return_val_if_fail(FL_IS_ATSPI_MONITOR(self), TRUE);
  return !self->filtering || self->serving_request;
}

void fl_atspi_monitor_start(FlAtspiMonitor* self) {
  g_return_if_fail(FL_IS_ATSPI_MONITOR(self));
  // at-spi2-atk's own check: with NO_AT_BRIDGE=1 GTK publishes nothing, so
  // no assistive client can read this application.
  const gchar* no_bridge = g_getenv("NO_AT_BRIDGE");
  if (no_bridge != nullptr && atoi(no_bridge) == 1) {
    return;
  }
  g_bus_get(G_BUS_TYPE_SESSION, self->cancellable, session_ready_cb,
            weak_box_new(self));
}
