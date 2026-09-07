// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Included first as it collides with the X11 headers.
#include "flutter/shell/platform/linux/testing/linux_test.h"
#include "gtest/gtest.h"

#include "flutter/shell/platform/linux/fl_view_accessible.h"
#include "flutter/shell/platform/linux/fl_view_private.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_engine.h"
#include "flutter/shell/platform/linux/testing/mock_gtk.h"
#include "flutter/shell/platform/linux/testing/mock_signal_handler.h"

class FlViewAccessibleTest : public flutter::testing::LinuxTest {};

TEST_F(FlViewAccessibleTest, GtkViewExposesSemanticTreeDirectly) {
  g_autoptr(FlView) view = fl_view_new(project);
  g_object_ref_sink(view);
  AtkObject* accessible = gtk_widget_get_accessible(GTK_WIDGET(view));
  ASSERT_TRUE(FL_IS_VIEW_ACCESSIBLE(accessible));
  EXPECT_EQ(accessible, ATK_OBJECT(fl_view_get_accessible(view)));
  EXPECT_EQ(gtk_accessible_get_widget(GTK_ACCESSIBLE(accessible)),
            GTK_WIDGET(view));
  EXPECT_FALSE(ATK_IS_SOCKET(accessible));
  EXPECT_FALSE(ATK_IS_PLUG(accessible));
  EXPECT_EQ(atk_object_get_n_accessible_children(accessible), 0);
  g_autoptr(AtkStateSet) initial_states = atk_object_ref_state_set(accessible);
  EXPECT_NE(initial_states, nullptr);

  FlutterSemanticsFlags flags = {};
  int32_t children[] = {1};
  FlutterSemanticsNode2 root = {
      .id = 0,
      .label = "Readable root",
      .rect = {.left = 7, .top = 11, .right = 647, .bottom = 491},
      .child_count = 1,
      .children_in_traversal_order = children,
      .flags2 = &flags};
  FlutterSemanticsNode2 leaf = {
      .id = 1,
      .label = "Readable child",
      .rect = {.left = 13, .top = 17, .right = 113, .bottom = 67},
      .flags2 = &flags};
  FlutterSemanticsNode2* nodes[] = {&root, &leaf};
  FlutterSemanticsUpdate2 update = {.node_count = 2, .nodes = nodes};
  fl_view_accessible_handle_update_semantics(FL_VIEW_ACCESSIBLE(accessible),
                                             &update);
  ASSERT_EQ(atk_object_get_n_accessible_children(accessible), 1);
  g_autoptr(AtkObject) child = atk_object_ref_accessible_child(accessible, 0);
  EXPECT_STREQ(atk_object_get_name(child), "Readable root");
  EXPECT_EQ(atk_object_get_parent(child), accessible);
  EXPECT_FALSE(ATK_IS_SOCKET(child));
  EXPECT_FALSE(ATK_IS_PLUG(child));

  // Both wrapper and semantic bounds stay in the widget hierarchy. Neither
  // query can synchronously forward to a plug on this same D-Bus owner.
  gint x = 0, y = 0, width = 0, height = 0;
  atk_component_get_extents(ATK_COMPONENT(accessible), &x, &y, &width, &height,
                            ATK_XY_SCREEN);
  gint view_x = x, view_y = y;
  atk_component_get_extents(ATK_COMPONENT(child), &x, &y, &width, &height,
                            ATK_XY_SCREEN);
  EXPECT_EQ(x, view_x + 7);
  EXPECT_EQ(y, view_y + 11);
  EXPECT_EQ(width, 640);
  EXPECT_EQ(height, 480);
  g_autoptr(AtkObject) grandchild = atk_object_ref_accessible_child(child, 0);
  ASSERT_NE(grandchild, nullptr);
  atk_component_get_extents(ATK_COMPONENT(grandchild), &x, &y, &width, &height,
                            ATK_XY_SCREEN);
  EXPECT_EQ(x, view_x + 20);
  EXPECT_EQ(y, view_y + 28);
  EXPECT_EQ(width, 100);
  EXPECT_EQ(height, 50);

  // Assistive clients may retain the tree after its GTK view is destroyed.
  // The container's lifecycle states come from GTK, not its semantic child.
  g_object_ref(accessible);
  fl_gtk_widget_destroy(GTK_WIDGET(view));
  EXPECT_EQ(gtk_accessible_get_widget(GTK_ACCESSIBLE(accessible)), nullptr);
  g_autoptr(AtkStateSet) final_states = atk_object_ref_state_set(accessible);
  EXPECT_TRUE(atk_state_set_contains_state(final_states, ATK_STATE_DEFUNCT));
  g_object_unref(accessible);
}

TEST_F(FlViewAccessibleTest, BuildTree) {
  g_autoptr(FlViewAccessible) accessible = fl_view_accessible_new(engine, 456);

  int32_t children[] = {111, 222};
  FlutterSemanticsFlags flags = {};
  FlutterSemanticsNode2 root_node = {.id = 0,
                                     .label = "root",
                                     .child_count = 2,
                                     .children_in_traversal_order = children,
                                     .flags2 = &flags,
                                     .identifier = ""};
  FlutterSemanticsNode2 child1_node = {
      .id = 111, .label = "child 1", .flags2 = &flags};
  FlutterSemanticsNode2 child2_node = {
      .id = 222, .label = "child 2", .flags2 = &flags};
  FlutterSemanticsNode2* nodes[3] = {&root_node, &child1_node, &child2_node};
  FlutterSemanticsUpdate2 update = {.node_count = 3, .nodes = nodes};
  fl_view_accessible_handle_update_semantics(accessible, &update);

  AtkObject* root_object =
      atk_object_ref_accessible_child(ATK_OBJECT(accessible), 0);
  EXPECT_STREQ(atk_object_get_name(root_object), "root");
  EXPECT_EQ(atk_object_get_index_in_parent(root_object), 0);
  EXPECT_EQ(atk_object_get_n_accessible_children(root_object), 2);

  AtkObject* child1_object = atk_object_ref_accessible_child(root_object, 0);
  EXPECT_STREQ(atk_object_get_name(child1_object), "child 1");
  EXPECT_EQ(atk_object_get_parent(child1_object), root_object);
  EXPECT_EQ(atk_object_get_index_in_parent(child1_object), 0);
  EXPECT_EQ(atk_object_get_n_accessible_children(child1_object), 0);

  AtkObject* child2_object = atk_object_ref_accessible_child(root_object, 1);
  EXPECT_STREQ(atk_object_get_name(child2_object), "child 2");
  EXPECT_EQ(atk_object_get_parent(child2_object), root_object);
  EXPECT_EQ(atk_object_get_index_in_parent(child2_object), 1);
  EXPECT_EQ(atk_object_get_n_accessible_children(child2_object), 0);
}

TEST_F(FlViewAccessibleTest, AddRemoveChildren) {
  g_autoptr(FlViewAccessible) accessible = fl_view_accessible_new(engine, 456);

  FlutterSemanticsFlags flags = {};
  FlutterSemanticsNode2 root_node = {
      .id = 0, .label = "root", .child_count = 0, .flags2 = &flags};
  FlutterSemanticsNode2* nodes[1] = {&root_node};
  FlutterSemanticsUpdate2 update = {.node_count = 1, .nodes = nodes};
  fl_view_accessible_handle_update_semantics(accessible, &update);

  AtkObject* root_object =
      atk_object_ref_accessible_child(ATK_OBJECT(accessible), 0);
  EXPECT_EQ(atk_object_get_n_accessible_children(root_object), 0);

  // add child1
  AtkObject* child1_object = nullptr;
  FlutterSemanticsNode2 child1_node = {
      .id = 111, .label = "child 1", .flags2 = &flags};
  {
    flutter::testing::MockSignalHandler2<gint, AtkObject*> child1_added(
        root_object, "children-changed::add");
    EXPECT_SIGNAL2(child1_added, ::testing::Eq(0), ::testing::A<AtkObject*>())
        .WillOnce(::testing::SaveArg<1>(&child1_object));

    int32_t children[] = {111};
    root_node.child_count = 1;
    root_node.children_in_traversal_order = children;
    FlutterSemanticsNode2* nodes[2] = {&root_node, &child1_node};
    FlutterSemanticsUpdate2 update = {.node_count = 2, .nodes = nodes};
    fl_view_accessible_handle_update_semantics(accessible, &update);
  }

  EXPECT_EQ(atk_object_get_n_accessible_children(root_object), 1);
  EXPECT_EQ(atk_object_ref_accessible_child(root_object, 0), child1_object);

  EXPECT_STREQ(atk_object_get_name(child1_object), "child 1");
  EXPECT_EQ(atk_object_get_parent(child1_object), root_object);
  EXPECT_EQ(atk_object_get_index_in_parent(child1_object), 0);
  EXPECT_EQ(atk_object_get_n_accessible_children(child1_object), 0);

  // add child2
  AtkObject* child2_object = nullptr;
  FlutterSemanticsNode2 child2_node = {
      .id = 222, .label = "child 2", .flags2 = &flags};
  {
    flutter::testing::MockSignalHandler2<gint, AtkObject*> child2_added(
        root_object, "children-changed::add");
    EXPECT_SIGNAL2(child2_added, ::testing::Eq(1), ::testing::A<AtkObject*>())
        .WillOnce(::testing::SaveArg<1>(&child2_object));

    int32_t children[] = {111, 222};
    root_node.child_count = 2;
    root_node.children_in_traversal_order = children;
    FlutterSemanticsNode2* nodes[3] = {&root_node, &child1_node, &child2_node};
    FlutterSemanticsUpdate2 update = {.node_count = 3, .nodes = nodes};
    fl_view_accessible_handle_update_semantics(accessible, &update);
  }

  EXPECT_EQ(atk_object_get_n_accessible_children(root_object), 2);
  EXPECT_EQ(atk_object_ref_accessible_child(root_object, 0), child1_object);
  EXPECT_EQ(atk_object_ref_accessible_child(root_object, 1), child2_object);

  EXPECT_STREQ(atk_object_get_name(child1_object), "child 1");
  EXPECT_EQ(atk_object_get_parent(child1_object), root_object);
  EXPECT_EQ(atk_object_get_index_in_parent(child1_object), 0);
  EXPECT_EQ(atk_object_get_n_accessible_children(child1_object), 0);

  EXPECT_STREQ(atk_object_get_name(child2_object), "child 2");
  EXPECT_EQ(atk_object_get_parent(child2_object), root_object);
  EXPECT_EQ(atk_object_get_index_in_parent(child2_object), 1);
  EXPECT_EQ(atk_object_get_n_accessible_children(child2_object), 0);

  // remove child1
  {
    flutter::testing::MockSignalHandler2<gint, AtkObject*> child1_removed(
        root_object, "children-changed::remove");
    EXPECT_SIGNAL2(child1_removed, ::testing::Eq(0),
                   ::testing::Eq(child1_object));

    const int32_t children[] = {222};
    root_node.child_count = 1;
    root_node.children_in_traversal_order = children;
    FlutterSemanticsNode2* nodes[3] = {&root_node, &child2_node};
    FlutterSemanticsUpdate2 update = {.node_count = 2, .nodes = nodes};
    fl_view_accessible_handle_update_semantics(accessible, &update);
  }

  EXPECT_EQ(atk_object_get_n_accessible_children(root_object), 1);
  EXPECT_EQ(atk_object_ref_accessible_child(root_object, 0), child2_object);

  EXPECT_STREQ(atk_object_get_name(child2_object), "child 2");
  EXPECT_EQ(atk_object_get_parent(child2_object), root_object);
  EXPECT_EQ(atk_object_get_index_in_parent(child2_object), 0);
  EXPECT_EQ(atk_object_get_n_accessible_children(child2_object), 0);

  // remove child2
  {
    flutter::testing::MockSignalHandler2<gint, AtkObject*> child2_removed(
        root_object, "children-changed::remove");
    EXPECT_SIGNAL2(child2_removed, ::testing::Eq(0),
                   ::testing::Eq(child2_object));

    root_node.child_count = 0;
    FlutterSemanticsNode2* nodes[1] = {&root_node};
    FlutterSemanticsUpdate2 update = {.node_count = 1, .nodes = nodes};
    fl_view_accessible_handle_update_semantics(accessible, &update);
  }

  EXPECT_EQ(atk_object_get_n_accessible_children(root_object), 0);
}
