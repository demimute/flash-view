#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <system_error>

#include "viewer/core/format_probe.h"

namespace viewer::core {
namespace {

constexpr std::array png_magic{
    std::byte{0x89},
    std::byte{0x50},
    std::byte{0x4E},
    std::byte{0x47},
    std::byte{0x0D},
    std::byte{0x0A},
    std::byte{0x1A},
    std::byte{0x0A},
};
constexpr std::array text_bytes{
    std::byte{'n'},
    std::byte{'o'},
    std::byte{'t'},
    std::byte{'e'},
};

TEST(FormatProbeTest, DetectsJpegSignature) {
  constexpr std::array bytes{
      std::byte{0xFF},
      std::byte{0xD8},
      std::byte{0xFF},
      std::byte{0xE0},
  };

  EXPECT_EQ(probe_format(bytes), ImageFormat::jpeg);
}

TEST(FormatProbeTest, DetectsCompletePngSignature) {
  constexpr std::array bytes{
      std::byte{0x89},
      std::byte{0x50},
      std::byte{0x4E},
      std::byte{0x47},
      std::byte{0x0D},
      std::byte{0x0A},
      std::byte{0x1A},
      std::byte{0x0A},
  };

  EXPECT_EQ(probe_format(bytes), ImageFormat::png);
}

TEST(FormatProbeTest, DetectsBmpSignature) {
  constexpr std::array bytes{
      std::byte{0x42},
      std::byte{0x4D},
  };

  EXPECT_EQ(probe_format(bytes), ImageFormat::bmp);
}

TEST(FormatProbeTest, ReturnsUnknownForEmptyInput) {
  EXPECT_EQ(probe_format(std::span<const std::byte>{}),
            ImageFormat::unknown);
}

TEST(FormatProbeTest, ReturnsUnknownForUnrecognizedBytes) {
  constexpr std::array bytes{
      std::byte{0x00},
      std::byte{0x01},
      std::byte{0x02},
  };

  EXPECT_EQ(probe_format(bytes), ImageFormat::unknown);
}

TEST(FormatProbeTest, ReturnsUnknownWhenOneSignatureByteIsWrong) {
  constexpr std::array jpeg{
      std::byte{0xFF},
      std::byte{0xD8},
      std::byte{0x00},
  };
  constexpr std::array png{
      std::byte{0x89},
      std::byte{0x50},
      std::byte{0x4E},
      std::byte{0x47},
      std::byte{0x0D},
      std::byte{0x0A},
      std::byte{0x1A},
      std::byte{0x00},
  };
  constexpr std::array bmp{
      std::byte{0x42},
      std::byte{0x00},
  };

  EXPECT_EQ(probe_format(jpeg), ImageFormat::unknown);
  EXPECT_EQ(probe_format(png), ImageFormat::unknown);
  EXPECT_EQ(probe_format(bmp), ImageFormat::unknown);
}

class UniqueTempDirectory {
 public:
  UniqueTempDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("viewer-format-probe-" +
             std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count()));
    std::filesystem::create_directories(path_);
  }

  ~UniqueTempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

void write_bytes(const std::filesystem::path& path,
                 std::span<const std::byte> bytes) {
  std::ofstream output(path, std::ios::binary);
  ASSERT_TRUE(output.is_open());
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(output.good());
}

TEST(FormatProbeTest, DetectsPngAndBmpWithTrailingData) {
  constexpr std::array png{
      std::byte{0x89},
      std::byte{0x50},
      std::byte{0x4E},
      std::byte{0x47},
      std::byte{0x0D},
      std::byte{0x0A},
      std::byte{0x1A},
      std::byte{0x0A},
      std::byte{0xFF},
  };
  constexpr std::array bmp{
      std::byte{0x42},
      std::byte{0x4D},
      std::byte{0xFF},
  };

  EXPECT_EQ(probe_format(png), ImageFormat::png);
  EXPECT_EQ(probe_format(bmp), ImageFormat::bmp);
}

TEST(FormatProbeTest, DoesNotMatchTruncatedSignatures) {
  constexpr std::array jpeg_prefix{
      std::byte{0xFF},
      std::byte{0xD8},
  };
  constexpr std::array png_prefix{
      std::byte{0x89},
      std::byte{0x50},
      std::byte{0x4E},
      std::byte{0x47},
  };
  constexpr std::array bmp_prefix{
      std::byte{0x42},
  };

  EXPECT_EQ(probe_format(jpeg_prefix), ImageFormat::unknown);
  EXPECT_EQ(probe_format(png_prefix), ImageFormat::unknown);
  EXPECT_EQ(probe_format(bmp_prefix), ImageFormat::unknown);
}

TEST(FormatProbeTest, ProbeFileHeaderAcceptsSupportedMagic) {
  UniqueTempDirectory directory;
  const auto image = directory.path() / "image.bin";
  write_bytes(image, png_magic);

  const auto result = probe_file_header(image);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), ImageFormat::png);
}

TEST(FormatProbeTest, ProbeFileHeaderRejectsUnsupportedMagicWithoutDecode) {
  UniqueTempDirectory directory;
  const auto image = directory.path() / "image.tiff";
  write_bytes(image, text_bytes);

  const auto result = probe_file_header(image);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ErrorCode::unsupported_format);
}

TEST(FormatProbeTest, ProbeFileHeaderReportsIoErrorForMissingFile) {
  UniqueTempDirectory directory;

  const auto result = probe_file_header(directory.path() / "missing.png");

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ErrorCode::io_error);
}

}  // namespace
}  // namespace viewer::core
