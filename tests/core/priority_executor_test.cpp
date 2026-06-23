#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
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
  std::atomic<int> completion_order = 0;
  std::atomic<int> task_completion_order = 0;
  std::atomic<int> wait_completion_order = 0;

  ASSERT_TRUE(executor.submit(Priority::current_image, [&] {
    task_started.set_value();
    release.wait();
    task_completion_order = ++completion_order;
  }));
  task_started.get_future().wait();

  auto wait_result = std::async(std::launch::async, [&] {
    executor.wait_idle();
    wait_completion_order = ++completion_order;
  });
  PriorityExecutorTestPeer::wait_for_idle_waiters(executor, 1);
  EXPECT_EQ(wait_result.wait_for(0s), std::future_status::timeout);

  release_task.set_value();
  EXPECT_EQ(wait_result.wait_for(1s), std::future_status::ready);
  EXPECT_LT(task_completion_order.load(), wait_completion_order.load());
}

TEST(PriorityExecutorTest, StopWaitsForActiveTaskAndDrainsAcceptedQueue) {
  PriorityExecutor executor(1);
  std::promise<void> first_started;
  std::promise<void> release_first;
  auto release = release_first.get_future().share();
  std::atomic<int> completed = 0;
  std::promise<void> stop_entered;
  std::atomic<bool> stop_returned = false;

  ASSERT_TRUE(executor.submit(Priority::current_image, [&] {
    first_started.set_value();
    release.wait();
    ++completed;
  }));
  ASSERT_TRUE(executor.submit(Priority::background, [&] { ++completed; }));
  first_started.get_future().wait();

  auto stop_result = std::async(std::launch::async, [&] {
    stop_entered.set_value();
    executor.stop();
    stop_returned.store(true);
  });
  stop_entered.get_future().wait();
  while (executor.submit(Priority::background, [] {})) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(stop_returned.load());
  EXPECT_EQ(stop_result.wait_for(0s), std::future_status::timeout);

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
  auto started = task_started.get_future().share();
  std::promise<void> release_task;
  auto release = release_task.get_future().share();
  std::promise<PriorityExecutor*> executor_created;
  std::promise<void> begin_destruction;
  auto destroy = begin_destruction.get_future().share();

  auto lifetime = std::async(std::launch::async, [&] {
    auto executor = std::make_unique<PriorityExecutor>(1);
    EXPECT_TRUE(executor->submit(Priority::current_image, [&] {
      task_started.set_value();
      release.wait();
    }));
    executor_created.set_value(executor.get());
    destroy.wait();
    executor.reset();
  });

  PriorityExecutor* executor = executor_created.get_future().get();
  started.wait();
  begin_destruction.set_value();
  PriorityExecutorTestPeer::wait_for_stop_waiters(*executor, 1);
  EXPECT_EQ(lifetime.wait_for(0s), std::future_status::timeout);
  release_task.set_value();
  EXPECT_EQ(lifetime.wait_for(1s), std::future_status::ready);
}

TEST(PriorityExecutorTest, StopDoesNotHoldLifecycleLockWhileJoining) {
  PriorityExecutor executor(1);
  std::promise<void> task_started;
  std::promise<void> release_task;
  auto release = release_task.get_future().share();
  std::promise<void> stop_entered;
  std::promise<void> start_entered;

  ASSERT_TRUE(executor.submit(Priority::current_image, [&] {
    task_started.set_value();
    release.wait();
  }));
  task_started.get_future().wait();

  auto stop_result = std::async(std::launch::async, [&] {
    stop_entered.set_value();
    executor.stop();
  });
  stop_entered.get_future().wait();
  while (executor.submit(Priority::background, [] {})) {
    std::this_thread::yield();
  }

  auto start_result = std::async(std::launch::async, [&] {
    start_entered.set_value();
    executor.start();
  });
  start_entered.get_future().wait();
  EXPECT_EQ(start_result.wait_for(1s), std::future_status::ready);

  release_task.set_value();
  EXPECT_EQ(stop_result.wait_for(1s), std::future_status::ready);
}

TEST(PriorityExecutorTest, ConcurrentStopsWaitForTheSameDrainAndJoin) {
  PriorityExecutor executor(1);
  std::promise<void> task_started;
  std::promise<void> release_task;
  auto release = release_task.get_future().share();

  ASSERT_TRUE(executor.submit(Priority::current_image, [&] {
    task_started.set_value();
    release.wait();
  }));
  task_started.get_future().wait();

  auto first_stop = std::async(std::launch::async, [&] {
    executor.stop();
  });
  PriorityExecutorTestPeer::wait_for_stop_waiters(executor, 1);

  auto second_stop = std::async(std::launch::async, [&] {
    executor.stop();
  });
  PriorityExecutorTestPeer::wait_for_stop_waiters(executor, 2);
  EXPECT_EQ(first_stop.wait_for(0s), std::future_status::timeout);
  EXPECT_EQ(second_stop.wait_for(0s), std::future_status::timeout);

  release_task.set_value();
  EXPECT_EQ(first_stop.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(second_stop.wait_for(1s), std::future_status::ready);
}

TEST(PriorityExecutorTest, WorkerStopRequestsDrainAndExternalStopJoins) {
  PriorityExecutor executor(1, false);
  std::promise<void> worker_stop_returned;
  std::promise<void> later_task_started;
  std::promise<void> release_later_task;
  auto release = release_later_task.get_future().share();

  ASSERT_TRUE(executor.submit(Priority::current_image, [&] {
    executor.stop();
    worker_stop_returned.set_value();
  }));
  ASSERT_TRUE(executor.submit(Priority::background, [&] {
    later_task_started.set_value();
    release.wait();
  }));

  executor.start();
  ASSERT_EQ(worker_stop_returned.get_future().wait_for(1s),
            std::future_status::ready);
  later_task_started.get_future().wait();

  auto external_stop = std::async(std::launch::async,
                                  [&] { executor.stop(); });
  PriorityExecutorTestPeer::wait_for_stop_waiters(executor, 1);
  EXPECT_EQ(external_stop.wait_for(0s), std::future_status::timeout);

  release_later_task.set_value();
  EXPECT_EQ(external_stop.wait_for(1s), std::future_status::ready);
}

TEST(PriorityExecutorTest, StartFailureJoinsCreatedThreadsAndLeavesStopped) {
  std::atomic<int> creation_count = 0;
  PriorityExecutor executor(
      3, false,
      [&](std::function<void()> worker) {
        if (creation_count++ == 1) {
          throw std::runtime_error("thread creation failed");
        }
        return std::thread(std::move(worker));
      });

  EXPECT_THROW(executor.start(), std::runtime_error);
  EXPECT_FALSE(executor.submit(Priority::background, [] {}));
  EXPECT_NO_THROW(executor.stop());
}

TEST(PriorityExecutorTest, ConstructorStartFailureCleansUpBeforeRethrowing) {
  std::atomic<int> creation_count = 0;

  EXPECT_THROW(
      PriorityExecutor(
          3, true,
          [&](std::function<void()> worker) {
            if (creation_count++ == 1) {
              throw std::runtime_error("thread creation failed");
            }
            return std::thread(std::move(worker));
          }),
      std::runtime_error);
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
