#include <gtest/gtest.h>

#include <chrono>

#include "viewer/core/load_metrics.h"

namespace viewer::core {
namespace {

TEST(LoadMetricsTest, ReportsDecodeAndTotalDurations) {
  using namespace std::chrono_literals;

  LoadMetrics metrics;
  const LoadMetrics::TimePoint start = LoadMetrics::Clock::now();
  metrics.requested = start;
  metrics.decode_started = start + 5ms;
  metrics.decode_finished = start + 35ms;
  metrics.presented = start + 50ms;

  EXPECT_EQ(metrics.decode_duration(), 30ms);
  EXPECT_EQ(metrics.total_duration(), 50ms);
}

TEST(LoadMetricsTest, SupportsRequestedThirtyAndPresentedFortyFiveMs) {
  using namespace std::chrono_literals;

  LoadMetrics metrics;
  const LoadMetrics::TimePoint start = LoadMetrics::Clock::now();
  metrics.requested = start;
  metrics.decode_started = start;
  metrics.decode_finished = start + 30ms;
  metrics.presented = start + 45ms;

  EXPECT_EQ(metrics.decode_duration(), 30ms);
  EXPECT_EQ(metrics.total_duration(), 45ms);
}

}  // namespace
}  // namespace viewer::core
