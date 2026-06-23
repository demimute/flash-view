#pragma once

#include <atomic>
#include <cstdint>

namespace viewer::core {

class LoadGeneration {
 public:
  [[nodiscard]] std::uint64_t begin() noexcept {
    return current_.fetch_add(1, std::memory_order_acq_rel) + 1;
  }

  [[nodiscard]] bool is_current(std::uint64_t value) const noexcept {
    return current_.load(std::memory_order_acquire) == value;
  }

  [[nodiscard]] std::uint64_t current() const noexcept {
    return current_.load(std::memory_order_acquire);
  }

 private:
  std::atomic_uint64_t current_{0};
};

}  // namespace viewer::core
