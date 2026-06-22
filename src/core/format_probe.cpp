#include "viewer/core/format_probe.h"

#include <algorithm>
#include <array>

namespace viewer::core {
namespace {

template <std::size_t Size>
bool begins_with(std::span<const std::byte> bytes,
                 const std::array<std::byte, Size>& signature) noexcept {
  return bytes.size() >= signature.size() &&
         std::equal(signature.begin(), signature.end(), bytes.begin());
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
      std::byte{'B'},
      std::byte{'M'},
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

}  // namespace viewer::core
