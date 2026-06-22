#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace viewer::core {

class CancellationToken {
 public:
  CancellationToken() : cancelled_(std::make_shared<std::atomic_bool>(false)) {}

  [[nodiscard]] bool is_cancelled() const noexcept {
    return cancelled_->load(std::memory_order_acquire);
  }

 private:
  explicit CancellationToken(std::shared_ptr<std::atomic_bool> cancelled)
      : cancelled_(std::move(cancelled)) {}

  std::shared_ptr<std::atomic_bool> cancelled_;

  friend class CancellationSource;
};

class CancellationSource {
 public:
  CancellationSource()
      : cancelled_(std::make_shared<std::atomic_bool>(false)) {}

  [[nodiscard]] CancellationToken token() const {
    return CancellationToken(cancelled_);
  }

  void cancel() noexcept {
    cancelled_->store(true, std::memory_order_release);
  }

 private:
  std::shared_ptr<std::atomic_bool> cancelled_;
};

}  // namespace viewer::core
