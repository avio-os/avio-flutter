// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/fl_semantics_demand.h"

#include "gtest/gtest.h"

namespace {

// Runs the default main context until |demand| is disabled or a bounded
// number of idle iterations passed.
void run_until_disabled(FlSemanticsDemand* demand) {
  for (int i = 0; i < 1000 && fl_semantics_demand_get_enabled(demand); i++) {
    g_main_context_iteration(nullptr, TRUE);
  }
}

// Runs every source that is ready now, without blocking.
void run_pending() {
  while (g_main_context_iteration(nullptr, FALSE)) {
  }
}

void count_notify(GObject* object, GParamSpec* pspec, gpointer user_data) {
  (*static_cast<int*>(user_data))++;
}

}  // namespace

TEST(FlSemanticsDemandTest, StartsDisabled) {
  g_autoptr(FlSemanticsDemand) demand = fl_semantics_demand_new(0);
  EXPECT_FALSE(fl_semantics_demand_get_enabled(demand));
  run_pending();
  EXPECT_FALSE(fl_semantics_demand_get_enabled(demand));
}

TEST(FlSemanticsDemandTest, AReadEnablesUntilTheHoldExpires) {
  g_autoptr(FlSemanticsDemand) demand = fl_semantics_demand_new(0);
  int notifications = 0;
  g_signal_connect(demand, "notify::enabled", G_CALLBACK(count_notify),
                   &notifications);

  fl_semantics_demand_note_read(demand);
  EXPECT_TRUE(fl_semantics_demand_get_enabled(demand));
  fl_semantics_demand_note_read(demand);
  EXPECT_EQ(notifications, 1);

  run_until_disabled(demand);
  EXPECT_FALSE(fl_semantics_demand_get_enabled(demand));
  EXPECT_EQ(notifications, 2);
}

TEST(FlSemanticsDemandTest, AHoldKeepsSemanticsAfterARead) {
  g_autoptr(FlSemanticsDemand) demand = fl_semantics_demand_new(60 * 1000);
  fl_semantics_demand_note_read(demand);
  run_pending();
  EXPECT_TRUE(fl_semantics_demand_get_enabled(demand));
}

TEST(FlSemanticsDemandTest, AStandingSourceOutlastsTheHold) {
  g_autoptr(FlSemanticsDemand) demand = fl_semantics_demand_new(0);
  fl_semantics_demand_note_read(demand);
  fl_semantics_demand_set_source(
      demand, FL_SEMANTICS_DEMAND_SOURCE_SCREEN_READER, TRUE);
  run_pending();
  EXPECT_TRUE(fl_semantics_demand_get_enabled(demand));

  // Another source ending changes nothing while one still wants semantics.
  fl_semantics_demand_set_source(demand, FL_SEMANTICS_DEMAND_SOURCE_LISTENERS,
                                 TRUE);
  fl_semantics_demand_set_source(demand, FL_SEMANTICS_DEMAND_SOURCE_LISTENERS,
                                 FALSE);
  run_pending();
  EXPECT_TRUE(fl_semantics_demand_get_enabled(demand));

  fl_semantics_demand_set_source(
      demand, FL_SEMANTICS_DEMAND_SOURCE_SCREEN_READER, FALSE);
  run_until_disabled(demand);
  EXPECT_FALSE(fl_semantics_demand_get_enabled(demand));
}

TEST(FlSemanticsDemandTest, ASourceEndingStartsTheHold) {
  g_autoptr(FlSemanticsDemand) demand = fl_semantics_demand_new(60 * 1000);
  fl_semantics_demand_set_source(demand, FL_SEMANTICS_DEMAND_SOURCE_LISTENERS,
                                 TRUE);
  fl_semantics_demand_set_source(demand, FL_SEMANTICS_DEMAND_SOURCE_LISTENERS,
                                 FALSE);
  run_pending();
  EXPECT_TRUE(fl_semantics_demand_get_enabled(demand));
}

TEST(FlSemanticsDemandTest, ASourceThatNeverWantedSemanticsEnablesNothing) {
  g_autoptr(FlSemanticsDemand) demand = fl_semantics_demand_new(0);
  fl_semantics_demand_set_source(
      demand, FL_SEMANTICS_DEMAND_SOURCE_SCREEN_READER, FALSE);
  fl_semantics_demand_set_source(demand, FL_SEMANTICS_DEMAND_SOURCE_LISTENERS,
                                 FALSE);
  EXPECT_FALSE(fl_semantics_demand_get_enabled(demand));
}

TEST(FlSemanticsDemandTest, AReadAfterExpiryEnablesAgain) {
  g_autoptr(FlSemanticsDemand) demand = fl_semantics_demand_new(0);
  fl_semantics_demand_note_read(demand);
  run_until_disabled(demand);
  ASSERT_FALSE(fl_semantics_demand_get_enabled(demand));

  fl_semantics_demand_note_read(demand);
  EXPECT_TRUE(fl_semantics_demand_get_enabled(demand));
  run_until_disabled(demand);
  EXPECT_FALSE(fl_semantics_demand_get_enabled(demand));
}
