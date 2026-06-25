#pragma once

#include <optional>

#include "viewer/core/result.h"

namespace viewer::app {

enum class RendererFailureAction {
  none,
  attempt_recovery,
  fatal,
};

[[nodiscard]] constexpr RendererFailureAction classify_renderer_failure(
    const std::optional<core::Error>& error) noexcept {
  if (!error.has_value()) {
    return RendererFailureAction::none;
  }
  if (error->code == core::ErrorCode::render_target_lost) {
    return RendererFailureAction::attempt_recovery;
  }
  return RendererFailureAction::fatal;
}

[[nodiscard]] constexpr bool should_flush_pending_load_metrics(
    bool draw_presented,
    bool draw_failed) noexcept {
  return draw_presented && !draw_failed;
}

}  // namespace viewer::app
