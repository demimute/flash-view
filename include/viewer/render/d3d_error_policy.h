#pragma once

namespace viewer::render {

enum class GraphicsOutcome {
  success,
  occluded,
  recreate_target,
  device_removed,
  device_reset,
  driver_internal_error,
  other_failure,
};

enum class DrawAction {
  drawn,
  render_target_lost,
  platform_error,
};

enum class PresentAction {
  presented,
  skipped,
  render_target_lost,
  platform_error,
};

enum class RendererDriver {
  hardware,
  warp,
};

enum class DeviceCreationAction {
  use_created_device,
  try_warp,
  render_target_lost,
  platform_error,
};

[[nodiscard]] constexpr bool is_lost_graphics_outcome(
    GraphicsOutcome outcome) noexcept {
  return outcome == GraphicsOutcome::recreate_target ||
         outcome == GraphicsOutcome::device_removed ||
         outcome == GraphicsOutcome::device_reset ||
         outcome == GraphicsOutcome::driver_internal_error;
}

[[nodiscard]] constexpr DrawAction classify_draw_outcome(
    GraphicsOutcome outcome) noexcept {
  if (outcome == GraphicsOutcome::success) {
    return DrawAction::drawn;
  }
  if (is_lost_graphics_outcome(outcome)) {
    return DrawAction::render_target_lost;
  }
  return DrawAction::platform_error;
}

[[nodiscard]] constexpr PresentAction classify_present_outcome(
    GraphicsOutcome outcome) noexcept {
  if (outcome == GraphicsOutcome::success) {
    return PresentAction::presented;
  }
  if (outcome == GraphicsOutcome::occluded) {
    return PresentAction::skipped;
  }
  if (is_lost_graphics_outcome(outcome)) {
    return PresentAction::render_target_lost;
  }
  return PresentAction::platform_error;
}

[[nodiscard]] constexpr DeviceCreationAction classify_device_creation_failure(
    GraphicsOutcome outcome,
    RendererDriver attempted_driver) noexcept {
  if (outcome == GraphicsOutcome::success) {
    return DeviceCreationAction::use_created_device;
  }
  if (is_lost_graphics_outcome(outcome)) {
    return DeviceCreationAction::render_target_lost;
  }
  if (attempted_driver == RendererDriver::hardware) {
    return DeviceCreationAction::try_warp;
  }
  return DeviceCreationAction::platform_error;
}

}  // namespace viewer::render
