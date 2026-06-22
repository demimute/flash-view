#include "viewer/core/priority_executor.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <queue>
#include <unordered_set>
#include <utility>

namespace viewer::core {

struct PriorityExecutor::State {
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
  Lifecycle lifecycle = Lifecycle::not_started;
};

namespace {

[[nodiscard]] PriorityExecutor::ThreadFactory default_thread_factory() {
  return [](std::function<void()> worker) {
    return std::thread(std::move(worker));
  };
}

}  // namespace

PriorityExecutor::PriorityExecutor(std::size_t thread_count,
                                   bool start_immediately)
    : PriorityExecutor(thread_count, start_immediately,
                       default_thread_factory()) {}

PriorityExecutor::PriorityExecutor(std::size_t thread_count,
                                   bool start_immediately,
                                   ThreadFactory thread_factory)
    : thread_count_(std::max<std::size_t>(thread_count, 1)),
      thread_factory_(std::move(thread_factory)),
      state_(std::make_shared<State>()) {
  if (start_immediately) {
    start();
  }
}

PriorityExecutor::~PriorityExecutor() {
  try {
    stop();
  } catch (...) {
  }
  finish_destruction();
}

void PriorityExecutor::start() {
  std::unique_lock lifecycle_lock(lifecycle_mutex_);
  {
    std::lock_guard state_lock(state_->mutex);
    if (state_->lifecycle != State::Lifecycle::not_started) {
      return;
    }
    state_->lifecycle = State::Lifecycle::running;
  }

  std::vector<std::thread> created;
  try {
    created.reserve(thread_count_);
    for (std::size_t index = 0; index < thread_count_; ++index) {
      created.push_back(thread_factory_(
          [state = state_] { worker_loop(state); }));
    }
  } catch (...) {
    {
      std::lock_guard state_lock(state_->mutex);
      state_->lifecycle = State::Lifecycle::stopping;
    }
    state_->work_available.notify_all();

    join_in_progress_ = true;
    lifecycle_lock.unlock();
    for (auto& worker : created) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    lifecycle_lock.lock();

    {
      std::lock_guard state_lock(state_->mutex);
      while (!state_->tasks.empty()) {
        state_->tasks.pop();
      }
      state_->lifecycle = State::Lifecycle::stopped;
      state_->idle.notify_all();
    }
    joined_ = true;
    join_in_progress_ = false;
    lifecycle_changed_.notify_all();
    throw;
  }

  workers_ = std::move(created);
}

void PriorityExecutor::stop() {
  const bool caller_is_worker = called_from_worker();
  std::unique_lock lifecycle_lock(lifecycle_mutex_);

  bool needs_workers = false;
  {
    std::lock_guard state_lock(state_->mutex);
    if (state_->lifecycle == State::Lifecycle::not_started) {
      needs_workers = !state_->tasks.empty();
      state_->lifecycle = needs_workers ? State::Lifecycle::running
                                        : State::Lifecycle::stopped;
      if (!needs_workers) {
        joined_ = true;
      }
    }
  }

  if (needs_workers) {
    std::vector<std::thread> created;
    try {
      created.reserve(thread_count_);
      for (std::size_t index = 0; index < thread_count_; ++index) {
        created.push_back(thread_factory_(
            [state = state_] { worker_loop(state); }));
      }
      workers_ = std::move(created);
    } catch (...) {
      {
        std::lock_guard state_lock(state_->mutex);
        state_->lifecycle = State::Lifecycle::stopping;
      }
      state_->work_available.notify_all();

      join_in_progress_ = true;
      lifecycle_lock.unlock();
      for (auto& worker : created) {
        if (worker.joinable()) {
          worker.join();
        }
      }
      lifecycle_lock.lock();

      {
        std::lock_guard state_lock(state_->mutex);
        while (!state_->tasks.empty()) {
          state_->tasks.pop();
        }
        state_->lifecycle = State::Lifecycle::stopped;
        state_->idle.notify_all();
      }
      joined_ = true;
      join_in_progress_ = false;
      lifecycle_changed_.notify_all();
      throw;
    }
  }

  {
    std::lock_guard state_lock(state_->mutex);
    if (state_->lifecycle != State::Lifecycle::stopped) {
      state_->lifecycle = State::Lifecycle::stopping;
    }
  }
  state_->work_available.notify_all();

  if (caller_is_worker || joined_) {
    return;
  }

  if (join_in_progress_) {
    lifecycle_changed_.wait(lifecycle_lock,
                            [this] { return joined_; });
    return;
  }

  join_in_progress_ = true;
  auto workers = std::move(workers_);
  lifecycle_lock.unlock();

  for (auto& worker : workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }

  lifecycle_lock.lock();
  {
    std::lock_guard state_lock(state_->mutex);
    state_->lifecycle = State::Lifecycle::stopped;
    state_->idle.notify_all();
  }
  joined_ = true;
  join_in_progress_ = false;
  lifecycle_changed_.notify_all();
}

void PriorityExecutor::wait_idle() {
  {
    std::unique_lock state_lock(state_->mutex);
    if (state_->lifecycle == State::Lifecycle::not_started &&
        !state_->tasks.empty()) {
      state_lock.unlock();
      start();
      state_lock.lock();
    }
    state_->idle.wait(state_lock, [this] {
      return state_->tasks.empty() && state_->active_tasks == 0;
    });
  }
}

bool PriorityExecutor::submit(Priority priority,
                              std::function<void()> task) {
  {
    std::lock_guard state_lock(state_->mutex);
    if (state_->lifecycle == State::Lifecycle::stopping ||
        state_->lifecycle == State::Lifecycle::stopped) {
      return false;
    }
    state_->tasks.push(State::Task{
        .priority = priority,
        .sequence = state_->next_sequence++,
        .function = std::move(task),
    });
  }
  state_->work_available.notify_one();
  return true;
}

void PriorityExecutor::worker_loop(
    const std::shared_ptr<State>& state) {
  {
    std::lock_guard lock(state->mutex);
    state->worker_ids.insert(std::this_thread::get_id());
  }

  while (true) {
    std::function<void()> task;
    {
      std::unique_lock lock(state->mutex);
      state->work_available.wait(lock, [&] {
        return state->lifecycle == State::Lifecycle::stopping ||
               !state->tasks.empty();
      });
      if (state->tasks.empty()) {
        state->worker_ids.erase(std::this_thread::get_id());
        return;
      }

      task = std::move(state->tasks.top().function);
      state->tasks.pop();
      ++state->active_tasks;
    }

    try {
      task();
    } catch (...) {
    }

    {
      std::lock_guard lock(state->mutex);
      --state->active_tasks;
      if (state->tasks.empty() && state->active_tasks == 0) {
        state->idle.notify_all();
      }
    }
  }
}

bool PriorityExecutor::called_from_worker() const noexcept {
  std::lock_guard lock(state_->mutex);
  return state_->worker_ids.contains(std::this_thread::get_id());
}

void PriorityExecutor::finish_destruction() noexcept {
  std::vector<std::thread> workers;
  {
    std::unique_lock lifecycle_lock(lifecycle_mutex_);
    if (join_in_progress_) {
      lifecycle_changed_.wait(lifecycle_lock,
                              [this] { return joined_; });
      return;
    }
    workers = std::move(workers_);
  }

  const auto caller = std::this_thread::get_id();
  const bool caller_is_worker =
      std::any_of(workers.begin(), workers.end(),
                  [caller](const auto& worker) {
                    return worker.get_id() == caller;
                  });
  for (auto& worker : workers) {
    if (!worker.joinable()) {
      continue;
    }
    if (caller_is_worker) {
      worker.detach();
    } else {
      worker.join();
    }
  }
}

}  // namespace viewer::core
