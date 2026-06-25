#pragma once

#include <cstddef>
#include <filesystem>
#include <span>

#include "viewer/core/result.h"

namespace viewer::core {

enum class ImageFormat {
  unknown,
  jpeg,
  png,
  bmp,
};

[[nodiscard]] ImageFormat probe_format(
    std::span<const std::byte> bytes) noexcept;

[[nodiscard]] Result<ImageFormat> probe_file_header(
    const std::filesystem::path& path);

}  // namespace viewer::core
