#pragma once

#include <cstddef>
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
  std::size_t width = 0;
  std::size_t height = 0;
  std::size_t stride = 0;
  std::vector<std::byte> pixels;

  static Result<ImageFrame> allocate_bgra8(std::size_t width,
                                           std::size_t height,
                                           std::size_t budget) {
    constexpr std::size_t bytes_per_pixel = 4;

    if (width == 0 || height == 0) {
      return Result<ImageFrame>::failure(
          {ErrorCode::invalid_format, L"Image dimensions must be non-zero."});
    }

    if (width > std::numeric_limits<std::size_t>::max() / bytes_per_pixel) {
      return resource_limit(L"Image row stride exceeds addressable memory.");
    }
    const std::size_t stride = width * bytes_per_pixel;

    if (height > std::numeric_limits<std::size_t>::max() / stride) {
      return resource_limit(L"Image pixel storage exceeds addressable memory.");
    }
    const std::size_t pixel_bytes = stride * height;

    if (pixel_bytes > budget) {
      return resource_limit(L"Image pixel storage exceeds the memory budget.");
    }

    ImageFrame frame{
        .width = width,
        .height = height,
        .stride = stride,
    };
    try {
      frame.pixels.resize(pixel_bytes);
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

}  // namespace viewer::core
