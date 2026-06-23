#pragma once

#include <cstdint>
#include <optional>

namespace viewer::app {

inline constexpr unsigned kKeyLeft = 0x25U;
inline constexpr unsigned kKeyUp = 0x26U;
inline constexpr unsigned kKeyRight = 0x27U;
inline constexpr unsigned kKeyDown = 0x28U;
inline constexpr unsigned kKeySpace = 0x20U;
inline constexpr unsigned kKeyPageUp = 0x21U;
inline constexpr unsigned kKeyPageDown = 0x22U;

enum class KeyAction {
  none,
  previous,
  next,
  fit,
  one_to_one,
  rotate_clockwise,
};

[[nodiscard]] constexpr KeyAction classify_key(unsigned key) noexcept {
  switch (key) {
    case kKeyLeft:
    case kKeyUp:
    case kKeyPageUp:
      return KeyAction::previous;

    case kKeyRight:
    case kKeyDown:
    case kKeyPageDown:
    case kKeySpace:
      return KeyAction::next;

    case 'F':
    case 'f':
      return KeyAction::fit;

    case '1':
      return KeyAction::one_to_one;

    case 'R':
    case 'r':
      return KeyAction::rotate_clockwise;

    default:
      return KeyAction::none;
  }
}

class WheelDeltaAccumulator {
 public:
  [[nodiscard]] int consume(int delta) noexcept {
    partial_delta_ += delta;
    const int steps = partial_delta_ / kWheelDelta;
    partial_delta_ -= steps * kWheelDelta;
    return steps;
  }

 private:
  static constexpr int kWheelDelta = 120;
  int partial_delta_ = 0;
};

struct Point {
  int x = 0;
  int y = 0;
};

struct PanDelta {
  int dx = 0;
  int dy = 0;
};

class PanTracker {
 public:
  void begin(Point point) noexcept {
    panning_ = true;
    last_ = point;
  }

  [[nodiscard]] std::optional<PanDelta> move_to(Point point) noexcept {
    if (!panning_) {
      return std::nullopt;
    }

    const PanDelta delta{point.x - last_.x, point.y - last_.y};
    last_ = point;
    return delta;
  }

  void end() noexcept { panning_ = false; }

  [[nodiscard]] bool is_panning() const noexcept { return panning_; }

 private:
  bool panning_ = false;
  Point last_{};
};

}  // namespace viewer::app
