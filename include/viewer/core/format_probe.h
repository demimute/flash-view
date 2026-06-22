#pragma once

#include <cstddef>
#include <span>

namespace viewer::core {

enum class ImageFormat {
  unknown,
  jpeg,
  png,
  bmp,
};

[[nodiscard]] ImageFormat probe_format(
    std::span<const std::byte> bytes) noexcept;

}  // namespace viewer::core
