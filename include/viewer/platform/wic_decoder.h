#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "viewer/core/image_frame.h"
#include "viewer/core/result.h"

namespace viewer::platform {

namespace detail {

[[nodiscard]] bool should_fallback_to_wic1(long result) noexcept;
[[nodiscard]] bool is_io_error(long result) noexcept;

}  // namespace detail

class WicDecoder {
 public:
  [[nodiscard]] core::Result<core::ImageFrame> decode(
      const std::filesystem::path& path,
      std::size_t byte_budget) const;

  [[nodiscard]] core::Result<core::ImageFrame> decode_thumbnail(
      const std::filesystem::path& path,
      std::uint32_t max_edge,
      std::size_t byte_budget) const;

  [[nodiscard]] core::Result<core::AnimatedImage> decode_animation(
      const std::filesystem::path& path,
      std::size_t byte_budget) const;
};

}  // namespace viewer::platform
