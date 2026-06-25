#pragma once

#include <chrono>

namespace viewer::core {

struct LoadMetrics {
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  using Duration = Clock::duration;

  TimePoint requested{};
  TimePoint decode_started{};
  TimePoint decode_finished{};
  TimePoint presented{};

  [[nodiscard]] Duration decode_duration() const noexcept {
    return decode_finished - decode_started;
  }

  [[nodiscard]] Duration total_duration() const noexcept {
    return presented - requested;
  }
};

}  // namespace viewer::core
