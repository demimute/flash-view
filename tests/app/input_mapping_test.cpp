#include "viewer/app/input_mapping.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <limits>

namespace viewer::app {
namespace {

TEST(InputMappingTest, MapsTask12NavigationKeys) {
  EXPECT_EQ(classify_key(kKeyLeft), KeyAction::previous);
  EXPECT_EQ(classify_key(kKeyUp), KeyAction::previous);
  EXPECT_EQ(classify_key(kKeyPageUp), KeyAction::previous);

  EXPECT_EQ(classify_key(kKeyRight), KeyAction::next);
  EXPECT_EQ(classify_key(kKeyDown), KeyAction::next);
  EXPECT_EQ(classify_key(kKeyPageDown), KeyAction::next);
  EXPECT_EQ(classify_key(kKeySpace), KeyAction::next);
}

TEST(InputMappingTest, MapsTask13InteractionKeysCaseInsensitively) {
  EXPECT_EQ(classify_key('F'), KeyAction::fit);
  EXPECT_EQ(classify_key('f'), KeyAction::fit);
  EXPECT_EQ(classify_key('1'), KeyAction::one_to_one);
  EXPECT_EQ(classify_key('R'), KeyAction::rotate_clockwise);
  EXPECT_EQ(classify_key('r'), KeyAction::rotate_clockwise);
}

TEST(InputMappingTest, LeavesUnknownKeysUnhandled) {
  EXPECT_EQ(classify_key('Z'), KeyAction::none);
  EXPECT_EQ(classify_key(0xFFFFU), KeyAction::none);
}

TEST(InputMappingTest, WheelAccumulatorConvertsFullDeltasToSteps) {
  WheelDeltaAccumulator wheel;

  EXPECT_EQ(wheel.consume(120), 1);
  EXPECT_EQ(wheel.consume(-120), -1);
  EXPECT_EQ(wheel.consume(240), 2);
  EXPECT_EQ(wheel.consume(-360), -3);
}

TEST(InputMappingTest, WheelAccumulatorKeepsPartialDelta) {
  WheelDeltaAccumulator wheel;

  EXPECT_EQ(wheel.consume(60), 0);
  EXPECT_EQ(wheel.consume(59), 0);
  EXPECT_EQ(wheel.consume(1), 1);
  EXPECT_EQ(wheel.consume(-30), 0);
  EXPECT_EQ(wheel.consume(-90), -1);
}

TEST(InputMappingTest, WheelAccumulatorHandlesExtremePositiveDeltaSafely) {
  WheelDeltaAccumulator wheel;

  EXPECT_EQ(wheel.consume(std::numeric_limits<int>::max()), 17895697);
  EXPECT_EQ(wheel.pending_delta(), 7);
  EXPECT_LT(std::abs(wheel.pending_delta()), 120);
}

TEST(InputMappingTest, WheelAccumulatorHandlesExtremeNegativeDeltaSafely) {
  WheelDeltaAccumulator wheel;

  EXPECT_EQ(wheel.consume(std::numeric_limits<int>::min()), -17895697);
  EXPECT_EQ(wheel.pending_delta(), -8);
  EXPECT_LT(std::abs(wheel.pending_delta()), 120);
}

TEST(InputMappingTest, WheelAccumulatorKeepsPartialBoundedAcrossLargeDeltas) {
  WheelDeltaAccumulator wheel;

  EXPECT_EQ(wheel.consume(std::numeric_limits<int>::max()), 17895697);
  EXPECT_EQ(wheel.pending_delta(), 7);
  EXPECT_EQ(wheel.consume(std::numeric_limits<int>::max()), 17895697);
  EXPECT_EQ(wheel.pending_delta(), 14);
  EXPECT_EQ(wheel.consume(std::numeric_limits<int>::min()), -17895696);
  EXPECT_EQ(wheel.pending_delta(), -114);
  EXPECT_LT(std::abs(wheel.pending_delta()), 120);
}

TEST(InputMappingTest, PanTrackerReportsDeltasOnlyWhilePanning) {
  PanTracker pan;

  EXPECT_FALSE(pan.is_panning());
  EXPECT_FALSE(pan.move_to({3, 4}).has_value());

  pan.begin({10, 20});
  EXPECT_TRUE(pan.is_panning());

  const auto first = pan.move_to({13, 18});
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->dx, 3);
  EXPECT_EQ(first->dy, -2);

  const auto second = pan.move_to({20, 30});
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->dx, 7);
  EXPECT_EQ(second->dy, 12);

  pan.end();
  EXPECT_FALSE(pan.is_panning());
  EXPECT_FALSE(pan.move_to({25, 35}).has_value());
}

}  // namespace
}  // namespace viewer::app
