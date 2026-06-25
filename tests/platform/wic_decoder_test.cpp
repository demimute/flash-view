#include <gtest/gtest.h>

#include <Windows.h>
#include <objbase.h>

#include <cstddef>
#include <filesystem>
#include <string_view>

#include "viewer/core/result.h"
#include "viewer/platform/wic_decoder.h"

namespace viewer::platform {
namespace {

std::filesystem::path fixture_path(std::wstring_view filename) {
  return std::filesystem::path{TEST_FIXTURE_DIR} / filename;
}

TEST(WicDecoderTest, DecodesPngDimensionsAndStride) {
  const WicDecoder decoder;

  const auto result = decoder.decode(fixture_path(L"1x1.png"), 64U * 1024U);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().width, 1U);
  EXPECT_EQ(result.value().height, 1U);
  EXPECT_EQ(result.value().stride, 4U);
}

TEST(WicDecoderTest, DecodesPngToPremultipliedBgra) {
  const WicDecoder decoder;

  const auto result = decoder.decode(fixture_path(L"1x1.png"), 64U * 1024U);

  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result.value().pixels.size(), 4U);
  const auto channel = [&result](std::size_t index) {
    return std::to_integer<int>(result.value().pixels[index]);
  };
  EXPECT_NEAR(channel(0), 25, 1);
  EXPECT_NEAR(channel(1), 50, 1);
  EXPECT_NEAR(channel(2), 100, 1);
  EXPECT_EQ(channel(3), 128);
}

TEST(WicDecoderTest, DecodesBmpPixels) {
  const WicDecoder decoder;

  const auto result = decoder.decode(fixture_path(L"1x1.bmp"), 64U * 1024U);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().pixels.size(), 4U);
}

TEST(WicDecoderTest, ReportsCorruptInputAsDecodeError) {
  const WicDecoder decoder;

  const auto result =
      decoder.decode(fixture_path(L"corrupt.png"), 64U * 1024U);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, core::ErrorCode::decode_error);
}

TEST(WicDecoderTest, RejectsDecodedPixelsThatExceedBudget) {
  const WicDecoder decoder;

  const auto result = decoder.decode(fixture_path(L"1x1.png"), 3U);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, core::ErrorCode::resource_limit);
}

TEST(WicDecoderTest, ReportsMissingFileAsIoError) {
  const WicDecoder decoder;

  const auto result =
      decoder.decode(fixture_path(L"does-not-exist.png"), 64U * 1024U);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, core::ErrorCode::io_error);
}

TEST(WicDecoderTest, RepeatedDecodeMaintainsComLifecycle) {
  const WicDecoder decoder;

  for (int attempt = 0; attempt < 8; ++attempt) {
    const auto result =
        decoder.decode(fixture_path(L"1x1.png"), 64U * 1024U);
    ASSERT_TRUE(result.has_value()) << "attempt " << attempt;
    EXPECT_EQ(result.value().pixels.size(), 4U);
  }
}

TEST(WicDecoderHresultTest, FallsBackOnlyWhenWic2IsUnavailable) {
  EXPECT_TRUE(detail::should_fallback_to_wic1(REGDB_E_CLASSNOTREG));
  EXPECT_TRUE(detail::should_fallback_to_wic1(E_NOINTERFACE));
  EXPECT_FALSE(detail::should_fallback_to_wic1(E_OUTOFMEMORY));
  EXPECT_FALSE(detail::should_fallback_to_wic1(E_UNEXPECTED));
}

TEST(WicDecoderHresultTest, ClassifiesStorageSharingAndLockFailuresAsIo) {
  EXPECT_TRUE(detail::is_io_error(STG_E_SHAREVIOLATION));
  EXPECT_TRUE(detail::is_io_error(STG_E_LOCKVIOLATION));
  EXPECT_FALSE(detail::is_io_error(WINCODEC_ERR_BADIMAGE));
}

}  // namespace
}  // namespace viewer::platform
