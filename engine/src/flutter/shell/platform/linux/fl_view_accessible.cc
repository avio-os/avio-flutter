// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// GLib's C++ helpers use templates, so they must be included before the
// ATK aggregate header is wrapped in C linkage below.
#include <glib-object.h>
#include <glib.h>

// Workaround missing C++ linkage compatibility in ATK header.
// Fixed in https://gitlab.gnome.org/GNOME/at-spi2-core/-/merge_requests/219
extern "C" {
#include <atk/atk.h>
}

#include "flutter/shell/platform/linux/fl_accessible_node.h"
#include "flutter/shell/platform/linux/fl_accessible_text_field.h"
#include "flutter/shell/platform/linux/fl_engine_private.h"
#include "flutter/shell/platform/linux/fl_view_accessible.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_value.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_view.h"

static constexpr int32_t kRootSemanticsNodeId = 0;

struct _FlViewAccessible {
  GtkContainerAccessible parent_instance;

  GWeakRef engine;

  FlutterViewId view_id;

  // The nodes of the published tree, keyed by ID. Holds exactly the nodes
  // reachable from the root: an update drops every node it detaches.
  GHashTable* semantics_nodes_by_id;

  // Flag to track when root node is created.
  gboolean root_node_created;

  // The engine's semantics demand this view follows, and its handler.
  GWeakRef demand;
  gulong demand_handler;

  // Whether the engine builds semantics. An update that arrives while it
  // does not was sent before semantics were disabled and is dropped.
  gboolean semantics_enabled;

  // Nodes whose children name an ID that has no node. The tree is complete
  // when the root exists and no node is here.
  GHashTable* unresolved;

  // ATK_STATE_BUSY: semantics are enabled and the tree is not yet complete.
  gboolean busy;
};

G_DEFINE_TYPE(FlViewAccessible,
              fl_view_accessible,
              GTK_TYPE_CONTAINER_ACCESSIBLE)

// Enum copied from ATK 2.50, as the version we are building against doesn't
// have this.
typedef enum {
  FL_ATK_LIVE_NONE,
  FL_ATK_LIVE_POLITE,
  FL_ATK_LIVE_ASSERTIVE
} FlAtkLive;

static FlAccessibleNode* create_node(FlViewAccessible* self,
                                     FlutterSemanticsNode2* semantics) {
  g_autoptr(FlEngine) engine = FL_ENGINE(g_weak_ref_get(&self->engine));
  if (engine == nullptr) {
    return nullptr;
  }

  if (semantics->flags2->is_text_field) {
    return fl_accessible_text_field_new(engine, self->view_id, semantics->id);
  }

  return fl_accessible_node_new(engine, self->view_id, semantics->id);
}

static FlAccessibleNode* lookup_node(FlViewAccessible* self, int32_t id) {
  return FL_ACCESSIBLE_NODE(
      g_hash_table_lookup(self->semantics_nodes_by_id, GINT_TO_POINTER(id)));
}

// Something read this view: semantics demand when it serves an assistive
// client's request.
static void note_read(FlViewAccessible* self) {
  g_autoptr(FlEngine) engine = FL_ENGINE(g_weak_ref_get(&self->engine));
  if (engine != nullptr) {
    fl_engine_note_semantics_read(engine);
  }
}

// Reports BUSY whenever the view holds no complete tree, while semantics are
// off and until the first complete tree after they turn on, so a reader that
// finds no children knows a tree comes once it asks.
static void update_busy(FlViewAccessible* self) {
  gboolean complete = lookup_node(self, kRootSemanticsNodeId) != nullptr &&
                      g_hash_table_size(self->unresolved) == 0;
  gboolean busy = !complete;
  if (self->busy == busy) {
    return;
  }
  self->busy = busy;
  atk_object_notify_state_change(ATK_OBJECT(self), ATK_STATE_BUSY, busy);
}

// Drops the whole tree. Nothing of a tree the engine no longer maintains
// stays readable; the next read asks for semantics again.
static void clear_tree(FlViewAccessible* self) {
  g_hash_table_remove_all(self->unresolved);
  FlAccessibleNode* root = lookup_node(self, kRootSemanticsNodeId);
  if (root == nullptr) {
    g_hash_table_remove_all(self->semantics_nodes_by_id);
    return;
  }
  g_autoptr(FlAccessibleNode) removed = FL_ACCESSIBLE_NODE(g_object_ref(root));
  g_hash_table_remove_all(self->semantics_nodes_by_id);
  self->root_node_created = FALSE;
  g_signal_emit_by_name(self, "children-changed::remove", 0, removed, nullptr);
}

static void set_semantics_enabled(FlViewAccessible* self, gboolean enabled) {
  if (self->semantics_enabled != enabled) {
    self->semantics_enabled = enabled;
    if (!enabled) {
      clear_tree(self);
    }
  }
  update_busy(self);
}

static void demand_changed_cb(FlViewAccessible* self,
                              GParamSpec* pspec,
                              FlSemanticsDemand* demand) {
  set_semantics_enabled(self, fl_semantics_demand_get_enabled(demand));
}

// Gets the ATK node for the given id.
// If the node doesn't exist it will be created.
static FlAccessibleNode* get_node(FlViewAccessible* self,
                                  FlutterSemanticsNode2* semantics) {
  FlAccessibleNode* node = lookup_node(self, semantics->id);
  if (node != nullptr) {
    return node;
  }

  node = create_node(self, semantics);
  if (node == nullptr) {
    return nullptr;
  }
  if (semantics->id == kRootSemanticsNodeId) {
    fl_accessible_node_set_parent(node, ATK_OBJECT(self), 0);
  }
  g_hash_table_insert(self->semantics_nodes_by_id,
                      GINT_TO_POINTER(semantics->id),
                      reinterpret_cast<gpointer>(node));

  // Update when root node is created.
  if (!self->root_node_created && semantics->id == kRootSemanticsNodeId) {
    g_signal_emit_by_name(self, "children-changed::add", 0, node, nullptr);
    self->root_node_created = true;
  }

  return node;
}

// Implements AtkObject::get_n_children
static gint fl_view_accessible_get_n_children(AtkObject* accessible) {
  FlViewAccessible* self = FL_VIEW_ACCESSIBLE(accessible);
  note_read(self);
  FlAccessibleNode* node = lookup_node(self, 0);

  if (node == nullptr) {
    return 0;
  }

  return 1;
}

// Implements AtkObject::ref_child
static AtkObject* fl_view_accessible_ref_child(AtkObject* accessible, gint i) {
  FlViewAccessible* self = FL_VIEW_ACCESSIBLE(accessible);
  note_read(self);
  FlAccessibleNode* node = lookup_node(self, 0);

  if (i != 0 || node == nullptr) {
    return nullptr;
  }

  return ATK_OBJECT(g_object_ref(node));
}

// Implements AtkObject::get_role
static AtkRole fl_view_accessible_get_role(AtkObject* accessible) {
  return ATK_ROLE_PANEL;
}

// Implements AtkObject::ref_state_set. GTK owns the container's states;
// this view adds BUSY while its tree is being built.
static AtkStateSet* fl_view_accessible_ref_state_set(AtkObject* accessible) {
  FlViewAccessible* self = FL_VIEW_ACCESSIBLE(accessible);
  note_read(self);
  AtkStateSet* states = ATK_OBJECT_CLASS(fl_view_accessible_parent_class)
                            ->ref_state_set(accessible);
  if (self->busy) {
    atk_state_set_add_state(states, ATK_STATE_BUSY);
  }
  return states;
}

static void fl_view_accessible_dispose(GObject* object) {
  FlViewAccessible* self = FL_VIEW_ACCESSIBLE(object);

  g_autoptr(GObject) demand = G_OBJECT(g_weak_ref_get(&self->demand));
  if (demand != nullptr && self->demand_handler != 0) {
    g_signal_handler_disconnect(demand, self->demand_handler);
  }
  self->demand_handler = 0;
  g_weak_ref_clear(&self->demand);
  g_clear_pointer(&self->unresolved, g_hash_table_unref);
  g_clear_pointer(&self->semantics_nodes_by_id, g_hash_table_unref);

  g_weak_ref_clear(&self->engine);

  G_OBJECT_CLASS(fl_view_accessible_parent_class)->dispose(object);
}

static void fl_view_accessible_class_init(FlViewAccessibleClass* klass) {
  ATK_OBJECT_CLASS(klass)->get_n_children = fl_view_accessible_get_n_children;
  ATK_OBJECT_CLASS(klass)->ref_child = fl_view_accessible_ref_child;
  ATK_OBJECT_CLASS(klass)->get_role = fl_view_accessible_get_role;
  ATK_OBJECT_CLASS(klass)->ref_state_set = fl_view_accessible_ref_state_set;

  G_OBJECT_CLASS(klass)->dispose = fl_view_accessible_dispose;
}

static void fl_view_accessible_init(FlViewAccessible* self) {
  g_weak_ref_init(&self->engine, nullptr);
  g_weak_ref_init(&self->demand, nullptr);
  self->unresolved = g_hash_table_new(g_direct_hash, g_direct_equal);
  self->semantics_nodes_by_id = g_hash_table_new_full(
      g_direct_hash, g_direct_equal, nullptr, g_object_unref);
}

FlViewAccessible* fl_view_accessible_new(FlEngine* engine,
                                         FlutterViewId view_id) {
  FlViewAccessible* self =
      FL_VIEW_ACCESSIBLE(g_object_new(fl_view_accessible_get_type(), nullptr));
  fl_view_accessible_set_engine(self, engine, view_id);
  return self;
}

void fl_view_accessible_set_engine(FlViewAccessible* self,
                                   FlEngine* engine,
                                   FlutterViewId view_id) {
  g_return_if_fail(FL_IS_VIEW_ACCESSIBLE(self));
  g_return_if_fail(FL_IS_ENGINE(engine));
  g_weak_ref_set(&self->engine, engine);
  self->view_id = view_id;

  FlSemanticsDemand* demand = fl_engine_get_semantics_demand(engine);
  g_autoptr(GObject) previous = G_OBJECT(g_weak_ref_get(&self->demand));
  if (previous != G_OBJECT(demand)) {
    if (previous != nullptr && self->demand_handler != 0) {
      g_signal_handler_disconnect(previous, self->demand_handler);
    }
    self->demand_handler = g_signal_connect_object(
        demand, "notify::enabled", G_CALLBACK(demand_changed_cb), self,
        G_CONNECT_SWAPPED);
    g_weak_ref_set(&self->demand, demand);
  }
  set_semantics_enabled(self, fl_semantics_demand_get_enabled(demand));
}

// Drops |node| and every descendant still parented to it. A descendant that
// this update moved under another parent keeps its new place.
static void remove_subtree(FlViewAccessible* self, FlAccessibleNode* node) {
  g_autoptr(GPtrArray) pending = g_ptr_array_new();
  g_ptr_array_add(pending, node);
  while (pending->len > 0) {
    FlAccessibleNode* current = FL_ACCESSIBLE_NODE(
        g_ptr_array_remove_index_fast(pending, pending->len - 1));
    gint n_children = atk_object_get_n_accessible_children(ATK_OBJECT(current));
    for (gint i = 0; i < n_children; i++) {
      g_autoptr(AtkObject) child =
          atk_object_ref_accessible_child(ATK_OBJECT(current), i);
      if (FL_IS_ACCESSIBLE_NODE(child) &&
          atk_object_get_parent(child) == ATK_OBJECT(current)) {
        g_ptr_array_add(pending, child);
      }
    }
    // The map's reference keeps |current| alive until here; its children
    // queued above are still held by the map.
    g_hash_table_remove(self->unresolved, current);
    g_hash_table_remove(
        self->semantics_nodes_by_id,
        GINT_TO_POINTER(fl_accessible_node_get_node_id(current)));
  }
}

void fl_view_accessible_handle_update_semantics(
    FlViewAccessible* self,
    const FlutterSemanticsUpdate2* update) {
  g_return_if_fail(FL_IS_VIEW_ACCESSIBLE(self));
  if (!self->semantics_enabled) {
    return;
  }

  // Flutter's update carries only the nodes that changed. A node leaves the
  // tree when an updated parent stops naming it, and every parent whose
  // children change is in the update, so a node stays exactly when an updated
  // parent names it. These candidates are every child an updated parent
  // named before this update, and every node this update created.
  g_autoptr(GArray) candidates = g_array_new(FALSE, FALSE, sizeof(int32_t));
  g_autoptr(GHashTable) named = g_hash_table_new(g_direct_hash, g_direct_equal);

  g_autoptr(GHashTable) pending_children =
      g_hash_table_new_full(g_direct_hash, g_direct_equal, nullptr,
                            reinterpret_cast<GDestroyNotify>(fl_value_unref));
  for (size_t i = 0; i < update->node_count; i++) {
    FlutterSemanticsNode2* node = update->nodes[i];
    if (lookup_node(self, node->id) == nullptr &&
        node->id != kRootSemanticsNodeId) {
      g_array_append_val(candidates, node->id);
    }
    FlAccessibleNode* atk_node = get_node(self, node);
    if (atk_node == nullptr) {
      continue;
    }

    fl_accessible_node_set_flags(atk_node, node->flags2);
    fl_accessible_node_set_actions(atk_node, node->actions);
    fl_accessible_node_set_name(atk_node, node->label);
    fl_accessible_node_set_extents(
        atk_node, node->rect.left + node->transform.transX,
        node->rect.top + node->transform.transY,
        node->rect.right - node->rect.left, node->rect.bottom - node->rect.top);
    fl_accessible_node_set_value(atk_node, node->value);
    fl_accessible_node_set_text_selection(atk_node, node->text_selection_base,
                                          node->text_selection_extent);
    fl_accessible_node_set_text_direction(atk_node, node->text_direction);

    FlValue* children = fl_value_new_int32_list(
        node->children_in_traversal_order, node->child_count);
    g_hash_table_insert(pending_children, atk_node, children);
  }

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, pending_children);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    FlAccessibleNode* parent = FL_ACCESSIBLE_NODE(key);

    gint n_previous = atk_object_get_n_accessible_children(ATK_OBJECT(parent));
    for (gint i = 0; i < n_previous; i++) {
      g_autoptr(AtkObject) previous =
          atk_object_ref_accessible_child(ATK_OBJECT(parent), i);
      if (FL_IS_ACCESSIBLE_NODE(previous)) {
        int32_t id =
            fl_accessible_node_get_node_id(FL_ACCESSIBLE_NODE(previous));
        g_array_append_val(candidates, id);
      }
    }

    size_t child_count = fl_value_get_length(static_cast<FlValue*>(value));
    const int32_t* children_in_traversal_order =
        fl_value_get_int32_list(static_cast<FlValue*>(value));

    g_autoptr(GPtrArray) children = g_ptr_array_new();
    gboolean resolved = TRUE;
    for (size_t i = 0; i < child_count; i++) {
      FlAccessibleNode* child =
          lookup_node(self, children_in_traversal_order[i]);
      if (child == nullptr) {
        resolved = FALSE;
        continue;
      }
      g_hash_table_add(named, GINT_TO_POINTER(children_in_traversal_order[i]));
      fl_accessible_node_set_parent(child, ATK_OBJECT(parent), i);
      g_ptr_array_add(children, child);
    }
    fl_accessible_node_set_children(parent, children);
    if (resolved) {
      g_hash_table_remove(self->unresolved, parent);
    } else {
      g_hash_table_add(self->unresolved, parent);
    }
  }

  for (guint i = 0; i < candidates->len; i++) {
    int32_t id = g_array_index(candidates, int32_t, i);
    FlAccessibleNode* node = lookup_node(self, id);
    if (node != nullptr && id != kRootSemanticsNodeId &&
        !g_hash_table_contains(named, GINT_TO_POINTER(id))) {
      remove_subtree(self, node);
    }
  }
  update_busy(self);
}

void fl_view_accessible_send_announcement(FlViewAccessible* self,
                                          const char* message,
                                          gboolean assertive) {
  g_return_if_fail(FL_IS_VIEW_ACCESSIBLE(self));
  if (atk_get_major_version() == 2 && atk_get_minor_version() >= 50) {
    g_signal_emit_by_name(
        self, "notification", message,
        assertive ? FL_ATK_LIVE_ASSERTIVE : FL_ATK_LIVE_POLITE);
  } else if (atk_get_major_version() == 2 && atk_get_minor_version() >= 46) {
    g_signal_emit_by_name(self, "announcement", message);
  }
}
