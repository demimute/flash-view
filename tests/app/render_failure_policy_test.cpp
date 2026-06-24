#include "viewer/app/render_failure_policy.h"

#include <gtest/gtest.h>

#include "viewer/core/result.h"

namespace viewer::app {
namespace {

TEST(RenderFailurePolicyTest, SuccessfulOperationNeedsNoRecovery) {
  EXPECT_EQ(classify_renderer_failure(std::nullopt),
            RendererFailureAction::none);
}

TEST(RenderFailurePolicyTest, LostRenderTargetAttemptsRecovery) {
  EXPECT_EQ(classify_renderer_failure(core::Error{
                .code = core::ErrorCode::render_target_lost,
                .message = L"lost",
            }),
            RendererFailureAction::attempt_recovery);
}

TEST(RenderFailurePolicyTest, NonLostRendererFailureIsFatal) {
  EXPECT_EQ(classify_renderer_failure(core::Error{
                .code = core::ErrorCode::platform_error,
                .message = L"platform",
            }),
            RendererFailureAction::fatal);
}

TEST(RenderFailurePolicyTest, DrawMetricsFlushOnlyAfterActualPresent) {
  EXPECT_TRUE(should_flush_pending_load_metrics(true, false));
  EXPECT_FALSE(should_flush_pending_load_metrics(false, false));
  EXPECT_FALSE(should_flush_pending_load_metrics(true, true));
}

}  // namespace
}  // namespace viewer::app
