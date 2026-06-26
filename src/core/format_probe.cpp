#include "viewer/core/format_probe.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace viewer::core {
namespace {

constexpr std::size_t header_size = 32;

template <std::size_t Size>
bool begins_with(std::span<const std::byte> bytes,
                 const std::array<std::byte, Size>& signature) noexcept {
  return bytes.size() >= signature.size() &&
         std::equal(signature.begin(), signature.end(), bytes.begin());
}

bool brand_equals(std::span<const std::byte> bytes,
                  std::size_t offset,
                  std::string_view brand) noexcept {
  return brand.size() == 4 && bytes.size() >= offset + brand.size() &&
         bytes[offset] == static_cast<std::byte>(brand[0]) &&
         bytes[offset + 1] == static_cast<std::byte>(brand[1]) &&
         bytes[offset + 2] == static_cast<std::byte>(brand[2]) &&
         bytes[offset + 3] == static_cast<std::byte>(brand[3]);
}

bool is_avif_brand(std::span<const std::byte> bytes,
                   std::size_t offset) noexcept {
  return brand_equals(bytes, offset, "avif") ||
         brand_equals(bytes, offset, "avis");
}

bool is_heif_brand(std::span<const std::byte> bytes,
                   std::size_t offset) noexcept {
  return brand_equals(bytes, offset, "heic") ||
         brand_equals(bytes, offset, "heix") ||
         brand_equals(bytes, offset, "hevc") ||
         brand_equals(bytes, offset, "hevx") ||
         brand_equals(bytes, offset, "mif1") ||
         brand_equals(bytes, offset, "msf1");
}

ImageFormat classify_iso_base_media_format(
    std::span<const std::byte> bytes) noexcept {
  if (!brand_equals(bytes, 4, "ftyp")) {
    return ImageFormat::unknown;
  }
  if (is_avif_brand(bytes, 8)) {
    return ImageFormat::avif;
  }
  if (is_heif_brand(bytes, 8)) {
    return ImageFormat::heif;
  }
  for (std::size_t offset = 16; offset + 4 <= bytes.size(); offset += 4) {
    if (is_avif_brand(bytes, offset)) {
      return ImageFormat::avif;
    }
    if (is_heif_brand(bytes, offset)) {
      return ImageFormat::heif;
    }
  }
  return ImageFormat::unknown;
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
  constexpr std::array gif87_signature{
      std::byte{'G'},
      std::byte{'I'},
      std::byte{'F'},
      std::byte{'8'},
      std::byte{'7'},
      std::byte{'a'},
  };
  constexpr std::array gif89_signature{
      std::byte{'G'},
      std::byte{'I'},
      std::byte{'F'},
      std::byte{'8'},
      std::byte{'9'},
      std::byte{'a'},
  };
  constexpr std::array tiff_little_signature{
      std::byte{'I'},
      std::byte{'I'},
      std::byte{0x2A},
      std::byte{0x00},
  };
  constexpr std::array tiff_big_signature{
      std::byte{'M'},
      std::byte{'M'},
      std::byte{0x00},
      std::byte{0x2A},
  };
  constexpr std::array ico_signature{
      std::byte{0x00},
      std::byte{0x00},
      std::byte{0x01},
      std::byte{0x00},
  };
  constexpr std::array zip_signature{
      std::byte{'P'},
      std::byte{'K'},
      std::byte{0x03},
      std::byte{0x04},
  };
  constexpr std::array empty_zip_signature{
      std::byte{'P'},
      std::byte{'K'},
      std::byte{0x05},
      std::byte{0x06},
  };
  constexpr std::array spanned_zip_signature{
      std::byte{'P'},
      std::byte{'K'},
      std::byte{0x07},
      std::byte{0x08},
  };
  constexpr std::array seven_zip_signature{
      std::byte{'7'},
      std::byte{'z'},
      std::byte{0xBC},
      std::byte{0xAF},
      std::byte{0x27},
      std::byte{0x1C},
  };
  constexpr std::array rar4_signature{
      std::byte{'R'},
      std::byte{'a'},
      std::byte{'r'},
      std::byte{'!'},
      std::byte{0x1A},
      std::byte{0x07},
      std::byte{0x00},
  };
  constexpr std::array rar5_signature{
      std::byte{'R'},
      std::byte{'a'},
      std::byte{'r'},
      std::byte{'!'},
      std::byte{0x1A},
      std::byte{0x07},
      std::byte{0x01},
      std::byte{0x00},
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
  if (begins_with(bytes, gif87_signature) ||
      begins_with(bytes, gif89_signature)) {
    return ImageFormat::gif;
  }
  if (begins_with(bytes, tiff_little_signature) ||
      begins_with(bytes, tiff_big_signature)) {
    return ImageFormat::tiff;
  }
  if (begins_with(bytes, ico_signature)) {
    return ImageFormat::ico;
  }
  if (bytes.size() >= 12 && bytes[0] == std::byte{'R'} &&
      bytes[1] == std::byte{'I'} && bytes[2] == std::byte{'F'} &&
      bytes[3] == std::byte{'F'} && bytes[8] == std::byte{'W'} &&
      bytes[9] == std::byte{'E'} && bytes[10] == std::byte{'B'} &&
      bytes[11] == std::byte{'P'}) {
    return ImageFormat::webp;
  }
  if (bytes.size() >= 12 && brand_equals(bytes, 4, "ftyp")) {
    const ImageFormat iso_format = classify_iso_base_media_format(bytes);
    if (iso_format != ImageFormat::unknown) {
      return iso_format;
    }
  }
  if (bytes.size() >= 2 && bytes[0] == std::byte{0xFF} &&
      bytes[1] == std::byte{0x0A}) {
    return ImageFormat::jpeg_xl;
  }
  if (begins_with(bytes, zip_signature) ||
      begins_with(bytes, empty_zip_signature) ||
      begins_with(bytes, spanned_zip_signature)) {
    return ImageFormat::zip_archive;
  }
  if (begins_with(bytes, seven_zip_signature)) {
    return ImageFormat::seven_zip_archive;
  }
  if (begins_with(bytes, rar4_signature) ||
      begins_with(bytes, rar5_signature)) {
    return ImageFormat::rar_archive;
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

bool is_supported_image_format(ImageFormat format) noexcept {
  switch (format) {
    case ImageFormat::jpeg:
    case ImageFormat::png:
    case ImageFormat::bmp:
    case ImageFormat::gif:
    case ImageFormat::tiff:
    case ImageFormat::ico:
    case ImageFormat::webp:
    case ImageFormat::heif:
    case ImageFormat::avif:
    case ImageFormat::jpeg_xl:
      return true;
    case ImageFormat::zip_archive:
    case ImageFormat::seven_zip_archive:
    case ImageFormat::rar_archive:
    case ImageFormat::unknown:
      return false;
  }
  return false;
}

bool is_supported_archive_format(ImageFormat format) noexcept {
  switch (format) {
    case ImageFormat::zip_archive:
    case ImageFormat::seven_zip_archive:
    case ImageFormat::rar_archive:
      return true;
    case ImageFormat::jpeg:
    case ImageFormat::png:
    case ImageFormat::bmp:
    case ImageFormat::gif:
    case ImageFormat::tiff:
    case ImageFormat::ico:
    case ImageFormat::webp:
    case ImageFormat::heif:
    case ImageFormat::avif:
    case ImageFormat::jpeg_xl:
    case ImageFormat::unknown:
      return false;
  }
  return false;
}

namespace {

[[nodiscard]] std::wstring lower_extension(
    const std::filesystem::path& path) {
  std::wstring extension = path.extension().wstring();
  for (wchar_t& ch : extension) {
    ch = static_cast<wchar_t>(std::towlower(ch));
  }
  return extension;
}

}  // namespace

bool is_supported_image_extension(const std::filesystem::path& path) {
  const std::wstring extension = lower_extension(path);
  return extension == L".jpg" || extension == L".jpeg" ||
         extension == L".png" || extension == L".bmp" ||
         extension == L".gif" || extension == L".tif" ||
         extension == L".tiff" || extension == L".ico" ||
         extension == L".webp" || extension == L".heic" ||
         extension == L".heif" || extension == L".avif" ||
         extension == L".jxl";
}

bool is_supported_archive_extension(const std::filesystem::path& path) {
  const std::wstring extension = lower_extension(path);
  return extension == L".zip" || extension == L".cbz" ||
         extension == L".7z" || extension == L".cb7" ||
         extension == L".rar" || extension == L".cbr";
}

}  // namespace viewer::core
