#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>

#include "viewer/core/format_probe.h"

namespace viewer::core {
namespace {

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

}  // namespace
}  // namespace viewer::core
