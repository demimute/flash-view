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
  gif,
  tiff,
  ico,
  webp,
  heif,
  avif,
  jpeg_xl,
  zip_archive,
  seven_zip_archive,
  rar_archive,
};

[[nodiscard]] ImageFormat probe_format(
    std::span<const std::byte> bytes) noexcept;

[[nodiscard]] Result<ImageFormat> probe_file_header(
    const std::filesystem::path& path);

[[nodiscard]] bool is_supported_image_format(ImageFormat format) noexcept;

[[nodiscard]] bool is_supported_archive_format(ImageFormat format) noexcept;

[[nodiscard]] bool is_supported_image_extension(
    const std::filesystem::path& path);

[[nodiscard]] bool is_supported_archive_extension(
    const std::filesystem::path& path);

}  // namespace viewer::core
