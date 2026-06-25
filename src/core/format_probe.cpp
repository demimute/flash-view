#include "viewer/core/format_probe.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <span>
#include <utility>

namespace viewer::core {
namespace {

constexpr std::size_t header_size = 16;

template <std::size_t Size>
bool begins_with(std::span<const std::byte> bytes,
                 const std::array<std::byte, Size>& signature) noexcept {
  return bytes.size() >= signature.size() &&
         std::equal(signature.begin(), signature.end(), bytes.begin());
}

[[nodiscard]] Error io_error(std::wstring message) {
  return Error{
      .code = ErrorCode::io_error,
      .message = std::move(message),
  };
}

[[nodiscard]] Error unsupported_format(std::wstring message) {
  return Error{
      .code = ErrorCode::unsupported_format,
      .message = std::move(message),
  };
}

}  // namespace

ImageFormat probe_format(std::span<const std::byte> bytes) noexcept {
  constexpr std::array jpeg_signature{
      std::byte{0xFF},
      std::byte{0xD8},
      std::byte{0xFF},
  };
  constexpr std::array png_signature{
      std::byte{0x89},
      std::byte{0x50},
      std::byte{0x4E},
      std::byte{0x47},
      std::byte{0x0D},
      std::byte{0x0A},
      std::byte{0x1A},
      std::byte{0x0A},
  };
  constexpr std::array bmp_signature{
      std::byte{0x42},
      std::byte{0x4D},
  };

  if (begins_with(bytes, jpeg_signature)) {
    return ImageFormat::jpeg;
  }
  if (begins_with(bytes, png_signature)) {
    return ImageFormat::png;
  }
  if (begins_with(bytes, bmp_signature)) {
    return ImageFormat::bmp;
  }
  return ImageFormat::unknown;
}

Result<ImageFormat> probe_file_header(
    const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return Result<ImageFormat>::failure(
        io_error(L"Could not read the image header"));
  }

  std::array<std::byte, header_size> header{};
  input.read(reinterpret_cast<char*>(header.data()),
             static_cast<std::streamsize>(header.size()));
  const auto bytes_read = input.gcount();
  if (input.bad()) {
    return Result<ImageFormat>::failure(
        io_error(L"Could not read the image header"));
  }

  const ImageFormat format = probe_format(
      std::span<const std::byte>(header.data(),
                                 static_cast<std::size_t>(bytes_read)));
  if (format == ImageFormat::unknown) {
    return Result<ImageFormat>::failure(
        unsupported_format(L"Unsupported image format"));
  }
  return Result<ImageFormat>::success(format);
}

}  // namespace viewer::core
