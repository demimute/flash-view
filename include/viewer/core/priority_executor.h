#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace viewer::core {

enum class Priority {
  current_image = 0,
  visible_thumbnail = 1,
  adjacent_image = 2,
  near_thumbnail = 3,
  background = 4,
};

class PriorityExecutor {
 public:
  explicit PriorityExecutor(std::size_t thread_count,
                            bool start_immediately = true);
  ~PriorityExecutor();

  PriorityExecutor(const PriorityExecutor&) = delete;
  PriorityExecutor& operator=(const PriorityExecutor&) = delete;
  PriorityExecutor(PriorityExecutor&&) = delete;
  PriorityExecutor& operator=(PriorityExecutor&&) = delete;

  // start() and stop() are idempotent. stop() rejects new submissions, drains
  // all accepted tasks, and joins worker threads.
  void start();
  void stop();

  // Starts a delayed executor when queued work exists, then waits for both the
  // queue and the active task count to reach zero.
  void wait_idle();

  [[nodiscard]] bool submit(Priority priority, std::function<void()> task);

 private:
  struct Task {
    Priority priority;
    std::uint64_t sequence;
    std::function<void()> function;
  };

  struct TaskCompare {
    [[nodiscard]] bool operator()(const Task& lhs,
                                  const Task& rhs) const noexcept;
  };

  void worker_loop();
  [[nodiscard]] bool called_from_worker() const noexcept;

  const std::size_t thread_count_;
  std::mutex lifecycle_mutex_;
  mutable std::mutex mutex_;
  std::condition_variable work_available_;
  std::condition_variable idle_;
  std::priority_queue<Task, std::vector<Task>, TaskCompare> tasks_;
  std::vector<std::thread> workers_;
  std::uint64_t next_sequence_ = 0;
  std::size_t active_tasks_ = 0;
  bool started_ = false;
  bool stopping_ = false;
};

}  // namespace viewer::core
