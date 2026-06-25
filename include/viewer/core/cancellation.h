#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace viewer::core {

class CancellationToken {
 public:
  CancellationToken() : cancelled_(std::make_shared<std::atomic_bool>(false)) {}
  CancellationToken(const CancellationToken&) = default;
  CancellationToken& operator=(const CancellationToken&) = default;
  CancellationToken(CancellationToken&&) noexcept = default;
  CancellationToken& operator=(CancellationToken&&) noexcept = default;

  [[nodiscard]] bool is_cancelled() const noexcept {
    return cancelled_ && cancelled_->load(std::memory_order_acquire);
  }

 private:
  explicit CancellationToken(std::shared_ptr<std::atomic_bool> cancelled)
      : cancelled_(std::move(cancelled)) {}

  std::shared_ptr<std::atomic_bool> cancelled_;

  friend class CancellationSource;
};

class CancellationSource {
 public:
  CancellationSource() : cancelled_(make_state()) {}
  CancellationSource(const CancellationSource&) = default;
  CancellationSource& operator=(const CancellationSource&) = default;

  CancellationSource(CancellationSource&& other)
      : cancelled_(make_state()) {
    cancelled_.swap(other.cancelled_);
    if (!cancelled_) {
      cancelled_ = make_state();
    }
  }

  CancellationSource& operator=(CancellationSource&& other) {
    if (this == &other) {
      return *this;
    }

    auto replacement = make_state();
    std::shared_ptr<std::atomic_bool> fallback;
    if (!other.cancelled_) {
      fallback = make_state();
    }
    cancelled_ = std::move(other.cancelled_);
    other.cancelled_ = std::move(replacement);
    if (!cancelled_) {
      cancelled_ = std::move(fallback);
    }
    return *this;
  }

  [[nodiscard]] CancellationToken token() const {
    return cancelled_ ? CancellationToken(cancelled_) : CancellationToken();
  }

  void cancel() noexcept {
    if (cancelled_) {
      cancelled_->store(true, std::memory_order_release);
    }
  }

 private:
  static std::shared_ptr<std::atomic_bool> make_state() {
    return std::make_shared<std::atomic_bool>(false);
  }

  std::shared_ptr<std::atomic_bool> cancelled_;
};

}  // namespace viewer::core
