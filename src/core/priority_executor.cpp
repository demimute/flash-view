#include "viewer/core/priority_executor.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace viewer::core {

bool PriorityExecutor::TaskCompare::operator()(const Task& lhs,
                                               const Task& rhs) const noexcept {
  if (lhs.priority != rhs.priority) {
    return lhs.priority > rhs.priority;
  }
  return lhs.sequence > rhs.sequence;
}

PriorityExecutor::PriorityExecutor(std::size_t thread_count,
                                   bool start_immediately)
    : thread_count_(std::max<std::size_t>(thread_count, 1)) {
  if (start_immediately) {
    start();
  }
}

PriorityExecutor::~PriorityExecutor() { stop(); }

void PriorityExecutor::start() {
  std::lock_guard lifecycle_lock(lifecycle_mutex_);
  std::lock_guard lock(mutex_);
  if (started_ || stopping_) {
    return;
  }

  started_ = true;
  workers_.reserve(thread_count_);
  for (std::size_t index = 0; index < thread_count_; ++index) {
    workers_.emplace_back([this] { worker_loop(); });
  }
}

void PriorityExecutor::stop() {
  std::lock_guard lifecycle_lock(lifecycle_mutex_);
  {
    std::lock_guard lock(mutex_);
    if (!stopping_) {
      stopping_ = true;
      if (!started_ && !tasks_.empty()) {
        started_ = true;
        workers_.reserve(thread_count_);
        for (std::size_t index = 0; index < thread_count_; ++index) {
          workers_.emplace_back([this] { worker_loop(); });
        }
      }
    }
  }
  work_available_.notify_all();

  if (called_from_worker()) {
    return;
  }

  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

void PriorityExecutor::wait_idle() {
  {
    std::unique_lock lock(mutex_);
    if (!started_ && !stopping_ && !tasks_.empty()) {
      lock.unlock();
      start();
      lock.lock();
    }
    idle_.wait(lock,
               [this] { return tasks_.empty() && active_tasks_ == 0; });
  }
}

bool PriorityExecutor::submit(Priority priority,
                              std::function<void()> task) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return false;
    }
    tasks_.push(Task{
        .priority = priority,
        .sequence = next_sequence_++,
        .function = std::move(task),
    });
  }
  work_available_.notify_one();
  return true;
}

void PriorityExecutor::worker_loop() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock lock(mutex_);
      work_available_.wait(
          lock, [this] { return stopping_ || !tasks_.empty(); });
      if (tasks_.empty()) {
        return;
      }

      task = std::move(tasks_.top().function);
      tasks_.pop();
      ++active_tasks_;
    }

    try {
      task();
    } catch (...) {
    }

    {
      std::lock_guard lock(mutex_);
      --active_tasks_;
      if (tasks_.empty() && active_tasks_ == 0) {
        idle_.notify_all();
      }
    }
  }
}

bool PriorityExecutor::called_from_worker() const noexcept {
  const auto caller = std::this_thread::get_id();
  return std::any_of(workers_.begin(), workers_.end(),
                     [caller](const auto& worker) {
                       return worker.get_id() == caller;
                     });
}

}  // namespace viewer::core
