#include "viewer/render/d3d_error_policy.h"

#include <gtest/gtest.h>

namespace viewer::render {
namespace {

TEST(D3dErrorPolicyTest, PresentOcclusionIsNotAFailure) {
  EXPECT_EQ(classify_present_outcome(GraphicsOutcome::occluded),
            PresentAction::skipped);
}

TEST(D3dErrorPolicyTest, DeviceFailuresLoseTheRenderTarget) {
  EXPECT_EQ(classify_present_outcome(GraphicsOutcome::device_removed),
            PresentAction::render_target_lost);
  EXPECT_EQ(classify_present_outcome(GraphicsOutcome::device_reset),
            PresentAction::render_target_lost);
  EXPECT_EQ(classify_present_outcome(GraphicsOutcome::driver_internal_error),
            PresentAction::render_target_lost);
}

TEST(D3dErrorPolicyTest, OtherPresentFailuresStayPlatformErrors) {
  EXPECT_EQ(classify_present_outcome(GraphicsOutcome::success),
            PresentAction::presented);
  EXPECT_EQ(classify_present_outcome(GraphicsOutcome::other_failure),
            PresentAction::platform_error);
}

TEST(D3dErrorPolicyTest, Direct2DRecreateTargetLosesRenderTarget) {
  EXPECT_EQ(classify_draw_outcome(GraphicsOutcome::recreate_target),
            DrawAction::render_target_lost);
}

TEST(D3dErrorPolicyTest, OtherDrawFailuresStayPlatformErrors) {
  EXPECT_EQ(classify_draw_outcome(GraphicsOutcome::success),
            DrawAction::drawn);
  EXPECT_EQ(classify_draw_outcome(GraphicsOutcome::other_failure),
            DrawAction::platform_error);
}

}  // namespace
}  // namespace viewer::render
