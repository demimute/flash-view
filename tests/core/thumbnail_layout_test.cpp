#include "viewer/core/thumbnail_layout.h"

#include <gtest/gtest.h>

namespace viewer::core {
namespace {

TEST(ThumbnailLayoutTest, DefaultsToHiddenBottomDockWithoutPreview) {
  const ThumbnailLayoutState layout;

  EXPECT_FALSE(layout.visible);
  EXPECT_FALSE(layout.preview_visible);
  EXPECT_EQ(layout.dock, ThumbnailDock::bottom);
  EXPECT_EQ(layout.thumbnail_size, 128U);
}

TEST(ThumbnailLayoutTest, CyclesDockThroughAllPositions) {
  ThumbnailLayoutState layout;

  layout.cycle_dock();
  EXPECT_EQ(layout.dock, ThumbnailDock::left);
  layout.cycle_dock();
  EXPECT_EQ(layout.dock, ThumbnailDock::right);
  layout.cycle_dock();
  EXPECT_EQ(layout.dock, ThumbnailDock::top);
  layout.cycle_dock();
  EXPECT_EQ(layout.dock, ThumbnailDock::bottom);
}

TEST(ThumbnailLayoutTest, ClampsThumbnailSize) {
  ThumbnailLayoutState layout;

  for (int i = 0; i < 100; ++i) {
    layout.shrink_thumbnails();
  }
  EXPECT_EQ(layout.thumbnail_size, 48U);

  for (int i = 0; i < 100; ++i) {
    layout.grow_thumbnails();
  }
  EXPECT_EQ(layout.thumbnail_size, 320U);
}

TEST(ThumbnailLayoutTest, KeepsMinimumPanelExtentWithoutUpperLimit) {
  ThumbnailLayoutState layout;

  layout.resize_panel(20);
  EXPECT_EQ(layout.panel_extent, 96U);

  layout.resize_panel(320);
  EXPECT_EQ(layout.panel_extent, 320U);

  layout.resize_panel(900);
  EXPECT_EQ(layout.panel_extent, 900U);
}

}  // namespace
}  // namespace viewer::core
