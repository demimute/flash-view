#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

#include "viewer/core/result.h"

namespace viewer::core {

constexpr std::size_t operator""_MiB(unsigned long long value) {
  constexpr std::size_t bytes_per_mib = 1024U * 1024U;
  return static_cast<std::size_t>(value) * bytes_per_mib;
}

struct ImageFrame {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t stride = 0;
  std::vector<std::byte> pixels;

  static Result<ImageFrame> allocate_bgra8(std::uint32_t width,
                                           std::uint32_t height,
                                           std::size_t byte_budget) {
    constexpr std::uint32_t bytes_per_pixel = 4;

    if (width == 0 || height == 0) {
      return Result<ImageFrame>::failure(
          {ErrorCode::invalid_format, L"Image dimensions must be non-zero."});
    }

    if (width >
        (std::numeric_limits<std::uint32_t>::max)() / bytes_per_pixel) {
      return resource_limit(L"Image row stride exceeds the uint32 limit.");
    }
    const std::uint32_t stride = width * bytes_per_pixel;

    const std::uint64_t pixel_bytes =
        static_cast<std::uint64_t>(stride) * height;

    if (pixel_bytes > (std::numeric_limits<std::uint32_t>::max)()) {
      return resource_limit(L"Image pixel storage exceeds the uint32 limit.");
    }
    if (pixel_bytes > byte_budget) {
      return resource_limit(L"Image pixel storage exceeds the memory budget.");
    }

    ImageFrame frame{
        .width = width,
        .height = height,
        .stride = stride,
    };
    try {
      frame.pixels.resize(static_cast<std::size_t>(pixel_bytes));
    } catch (const std::bad_alloc&) {
      return resource_limit(L"Unable to allocate image pixel storage.");
    } catch (const std::length_error&) {
      return resource_limit(L"Image pixel storage exceeds vector capacity.");
    }

    return Result<ImageFrame>::success(std::move(frame));
  }

 private:
  static Result<ImageFrame> resource_limit(std::wstring message) {
    return Result<ImageFrame>::failure(
        {ErrorCode::resource_limit, std::move(message)});
  }
};

struct AnimatedImage {
  std::vector<ImageFrame> frames;
  std::vector<std::uint32_t> delays_ms;

  [[nodiscard]] bool animated() const noexcept {
    return frames.size() > 1 && frames.size() == delays_ms.size();
  }
};

}  // namespace viewer::core
