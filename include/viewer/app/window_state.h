#pragma once

namespace viewer::app {

class WindowState {
 public:
  [[nodiscard]] static constexpr bool should_resize(
      bool minimized, unsigned width, unsigned height) noexcept {
    return !minimized && width != 0 && height != 0;
  }
};

}  // namespace viewer::app
