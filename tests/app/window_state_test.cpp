#include "viewer/app/window_state.h"

#include <gtest/gtest.h>

namespace viewer::app {
namespace {

TEST(WindowStateTest, SkipsResizeWhileMinimized) {
  EXPECT_FALSE(WindowState::should_resize(true, 1200, 800));
}

TEST(WindowStateTest, SkipsResizeForZeroWidthOrHeight) {
  EXPECT_FALSE(WindowState::should_resize(false, 0, 800));
  EXPECT_FALSE(WindowState::should_resize(false, 1200, 0));
}

TEST(WindowStateTest, ResizesForVisibleNonZeroClientArea) {
  EXPECT_TRUE(WindowState::should_resize(false, 1200, 800));
}

}  // namespace
}  // namespace viewer::app
