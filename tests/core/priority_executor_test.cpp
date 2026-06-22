#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "viewer/core/priority_executor.h"

namespace viewer::core {
namespace {

using namespace std::chrono_literals;

static_assert(
    std::is_same_v<std::underlying_type_t<Priority>, std::uint8_t>);
static_assert(static_cast<int>(Priority::current_image) == 0);
static_assert(static_cast<int>(Priority::visible_thumbnail) == 1);
static_assert(static_cast<int>(Priority::adjacent_image) == 2);
static_assert(static_cast<int>(Priority::near_thumbnail) == 3);
static_assert(static_cast<int>(Priority::background) == 4);

TEST(PriorityExecutorTest, DelayedStartRunsByPriorityAndRejectsAfterStop) {
  PriorityExecutor executor(1, false);
  std::mutex results_mutex;
  std::vector<int> results;

  const auto record = [&](int value) {
    std::lock_guard lock(results_mutex);
    results.push_back(value);
  };

  EXPECT_TRUE(executor.submit(Priority::background, [&] { record(3); }));
  EXPECT_TRUE(executor.submit(Priority::current_image, [&] { record(1); }));
  EXPECT_TRUE(
      executor.submit(Priority::visible_thumbnail, [&] { record(2); }));

  executor.start();
  executor.wait_idle();
  executor.stop();

  EXPECT_EQ(results, (std::vector<int>{1, 2, 3}));
  EXPECT_FALSE(executor.submit(Priority::current_image, [] {}));
}

TEST(PriorityExecutorTest, EqualPriorityTasksRunInSubmissionOrder) {
  PriorityExecutor executor(1, false);
  std::vector<int> results;

  for (int value = 0; value < 8; ++value) {
    ASSERT_TRUE(executor.submit(Priority::background,
                                [&, value] { results.push_back(value); }));
  }

  executor.start();
  executor.wait_idle();

  EXPECT_EQ(results, (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}));
}

TEST(PriorityExecutorTest, WaitIdleWaitsForAnActiveTask) {
  PriorityExecutor executor(1);
  std::promise<void> task_started;
  std::promise<void> release_task;
  auto release = release_task.get_future().share();

  ASSERT_TRUE(executor.submit(Priority::current_image, [&] {
    task_started.set_value();
    release.wait();
  }));
  task_started.get_future().wait();

  auto wait_result =
      std::async(std::launch::async, [&] { executor.wait_idle(); });
  EXPECT_EQ(wait_result.wait_for(20ms), std::future_status::timeout);

  release_task.set_value();
  EXPECT_EQ(wait_result.wait_for(1s), std::future_status::ready);
}

TEST(PriorityExecutorTest, StopWaitsForActiveTaskAndDrainsAcceptedQueue) {
  PriorityExecutor executor(1);
  std::promise<void> first_started;
  std::promise<void> release_first;
  auto release = release_first.get_future().share();
  std::atomic<int> completed = 0;

  ASSERT_TRUE(executor.submit(Priority::current_image, [&] {
    first_started.set_value();
    release.wait();
    ++completed;
  }));
  ASSERT_TRUE(executor.submit(Priority::background, [&] { ++completed; }));
  first_started.get_future().wait();

  auto stop_result = std::async(std::launch::async, [&] { executor.stop(); });
  EXPECT_EQ(stop_result.wait_for(20ms), std::future_status::timeout);

  release_first.set_value();
  EXPECT_EQ(stop_result.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(completed.load(), 2);
}

TEST(PriorityExecutorTest, StopStartsAndDrainsDelayedExecutor) {
  PriorityExecutor executor(1, false);
  std::vector<int> results;

  ASSERT_TRUE(executor.submit(Priority::background,
                              [&] { results.push_back(2); }));
  ASSERT_TRUE(executor.submit(Priority::current_image,
                              [&] { results.push_back(1); }));

  executor.stop();

  EXPECT_EQ(results, (std::vector<int>{1, 2}));
}

TEST(PriorityExecutorTest, DestructorWaitsForRunningTask) {
  std::promise<void> task_started;
  std::promise<void> release_task;
  auto release = release_task.get_future().share();

  auto lifetime = std::async(std::launch::async, [&] {
    PriorityExecutor executor(1);
    EXPECT_TRUE(executor.submit(Priority::current_image, [&] {
      task_started.set_value();
      release.wait();
    }));
    task_started.get_future().wait();
  });

  EXPECT_EQ(lifetime.wait_for(20ms), std::future_status::timeout);
  release_task.set_value();
  EXPECT_EQ(lifetime.wait_for(1s), std::future_status::ready);
}

TEST(PriorityExecutorTest, TaskExceptionDoesNotStopLaterTasks) {
  PriorityExecutor executor(1, false);
  std::atomic<bool> later_task_ran = false;

  ASSERT_TRUE(executor.submit(Priority::current_image,
                              [] { throw std::runtime_error("task failed"); }));
  ASSERT_TRUE(executor.submit(Priority::background,
                              [&] { later_task_ran = true; }));

  executor.start();
  executor.wait_idle();

  EXPECT_TRUE(later_task_ran.load());
}

TEST(PriorityExecutorTest, ZeroThreadCountIsNormalizedToOne) {
  PriorityExecutor executor(0);
  std::atomic<bool> ran = false;

  ASSERT_TRUE(
      executor.submit(Priority::current_image, [&] { ran = true; }));
  executor.wait_idle();

  EXPECT_TRUE(ran.load());
}

TEST(PriorityExecutorTest, WaitIdleStartsDelayedExecutor) {
  PriorityExecutor executor(1, false);
  std::atomic<bool> ran = false;

  ASSERT_TRUE(
      executor.submit(Priority::current_image, [&] { ran = true; }));
  executor.wait_idle();

  EXPECT_TRUE(ran.load());
}

TEST(PriorityExecutorTest, RepeatedStartAndStopAreSafe) {
  PriorityExecutor executor(2, false);
  std::atomic<int> completed = 0;

  executor.start();
  executor.start();
  ASSERT_TRUE(
      executor.submit(Priority::current_image, [&] { ++completed; }));
  executor.wait_idle();
  executor.stop();
  executor.stop();

  EXPECT_EQ(completed.load(), 1);
  EXPECT_FALSE(executor.submit(Priority::background, [] {}));
}

}  // namespace
}  // namespace viewer::core
