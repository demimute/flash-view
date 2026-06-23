#pragma once

#include <memory>
#include <mutex>
#include <utility>

namespace viewer::core {

template <typename Destination, typename InstanceToken>
class CompletionGate {
 public:
  CompletionGate(Destination destination, InstanceToken instance_token,
                 bool accepting = true)
      : destination_(std::move(destination)),
        instance_token_(std::move(instance_token)),
        accepting_(accepting) {}

  void activate(Destination destination, InstanceToken instance_token) {
    std::scoped_lock lock(mutex_);
    destination_ = std::move(destination);
    instance_token_ = std::move(instance_token);
    accepting_ = true;
  }

  template <typename Payload, typename Publisher>
  [[nodiscard]] bool publish(std::unique_ptr<Payload>& payload,
                             Publisher&& publisher) {
    std::scoped_lock lock(mutex_);
    if (!accepting_) {
      return false;
    }
    if (!std::forward<Publisher>(publisher)(
            destination_, instance_token_, payload.get())) {
      return false;
    }
    payload.release();
    return true;
  }

  [[nodiscard]] bool close() {
    std::scoped_lock lock(mutex_);
    if (!accepting_) {
      return false;
    }
    accepting_ = false;
    destination_ = Destination{};
    return true;
  }

  [[nodiscard]] bool is_closed() const {
    std::scoped_lock lock(mutex_);
    return !accepting_;
  }

 private:
  mutable std::mutex mutex_;
  Destination destination_;
  InstanceToken instance_token_;
  bool accepting_ = true;
};

template <typename Context>
class AsyncShutdownState {
 public:
  explicit AsyncShutdownState(std::shared_ptr<Context> context)
      : context_(std::move(context)) {}

  [[nodiscard]] std::shared_ptr<Context> context() const {
    std::scoped_lock lock(mutex_);
    return context_;
  }

  [[nodiscard]] std::shared_ptr<Context> take_context() {
    std::scoped_lock lock(mutex_);
    return std::move(context_);
  }

 private:
  mutable std::mutex mutex_;
  std::shared_ptr<Context> context_;
};

}  // namespace viewer::core
