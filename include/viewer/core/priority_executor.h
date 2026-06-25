#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_set>
#include <vector>

namespace viewer::core {

enum class Priority : std::uint8_t {
  current_image = 0,
  visible_thumbnail = 1,
  adjacent_image = 2,
  near_thumbnail = 3,
  background = 4,
};

// PriorityExecutor must be owned and destroyed by a non-worker thread.
// Destruction must be synchronized with all member calls and must not occur
// concurrently with them. Destroying the executor from one of its own tasks is
// unsupported.
class PriorityExecutor {
 public:
  using ThreadFactory =
      std::function<std::thread(std::function<void()>)>;

  explicit PriorityExecutor(std::size_t thread_count,
                            bool start_immediately = true);
  PriorityExecutor(std::size_t thread_count, bool start_immediately,
                   ThreadFactory thread_factory);
  ~PriorityExecutor();

  PriorityExecutor(const PriorityExecutor&) = delete;
  PriorityExecutor& operator=(const PriorityExecutor&) = delete;
  PriorityExecutor(PriorityExecutor&&) = delete;
  PriorityExecutor& operator=(PriorityExecutor&&) = delete;

  // start() and stop() are idempotent. stop() rejects new submissions, drains
  // all accepted tasks, and joins worker threads when called externally.
  // A worker calling stop() only requests drain shutdown; a later external
  // stop() or destruction completes the joins.
  void start();
  void stop();

  // Starts a delayed executor when queued work exists, then waits for both the
  // queue and the active task count to reach zero.
  void wait_idle();

  [[nodiscard]] bool submit(Priority priority, std::function<void()> task);

 private:
  friend class PriorityExecutorTests;

  struct State {
    struct Task {
      Priority priority;
      std::uint64_t sequence;
      std::function<void()> function;
    };

    struct TaskCompare {
      [[nodiscard]] bool operator()(const Task& lhs,
                                    const Task& rhs) const noexcept {
        if (lhs.priority != rhs.priority) {
          return lhs.priority > rhs.priority;
        }
        return lhs.sequence > rhs.sequence;
      }
    };

    enum class Lifecycle {
      not_started,
      running,
      stopping,
      stopped,
    };

    std::mutex mutex;
    std::condition_variable work_available;
    std::condition_variable idle;
    std::priority_queue<Task, std::vector<Task>, TaskCompare> tasks;
    std::unordered_set<std::thread::id> worker_ids;
    std::uint64_t next_sequence = 0;
    std::size_t active_tasks = 0;
    std::size_t idle_waiter_count = 0;
    std::size_t stop_waiter_count = 0;
    Lifecycle lifecycle = Lifecycle::not_started;
  };

  static void worker_loop(const std::shared_ptr<State>& state);
  [[nodiscard]] bool called_from_worker() const noexcept;

  const std::size_t thread_count_;
  ThreadFactory thread_factory_;
  std::shared_ptr<State> state_;
  std::mutex lifecycle_mutex_;
  std::condition_variable lifecycle_changed_;
  std::vector<std::thread> workers_;
  bool join_in_progress_ = false;
  bool joined_ = false;
};

}  // namespace viewer::core
