#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

#include "viewer/core/cancellation.h"
#include "viewer/core/image_frame.h"
#include "viewer/core/load_request.h"

namespace viewer::core {
namespace {

TEST(ErrorCodeTest, DistinguishesLostRenderTargets) {
  constexpr Error error{
      .code = ErrorCode::render_target_lost,
      .message = L"lost",
  };

  EXPECT_EQ(error.code, ErrorCode::render_target_lost);
  EXPECT_NE(error.code, ErrorCode::platform_error);
}

TEST(LoadRequestTest, StoresPathAndGeneration) {
  const LoadRequest request{
      .path = L"C:\\images\\a.jpg",
      .generation = 42,
  };

  EXPECT_EQ(request.generation, 42U);
  EXPECT_EQ(request.path.filename(), L"a.jpg");
}

TEST(CancellationTest, SourceCancelsItsToken) {
  CancellationSource source;
  const CancellationToken token = source.token();

  EXPECT_FALSE(token.is_cancelled());

  source.cancel();

  EXPECT_TRUE(token.is_cancelled());
}

TEST(CancellationTest, TokenRemainsSafeAfterMoveConstructionAndAssignment) {
  CancellationSource source;
  source.cancel();
  CancellationToken original = source.token();

  CancellationToken moved(std::move(original));
  EXPECT_FALSE(original.is_cancelled());
  EXPECT_TRUE(moved.is_cancelled());

  CancellationToken assigned;
  assigned = std::move(moved);
  EXPECT_FALSE(moved.is_cancelled());
  EXPECT_TRUE(assigned.is_cancelled());
}

TEST(CancellationTest, SourceRemainsUsableAfterMoveConstruction) {
  CancellationSource original;
  const CancellationToken original_token = original.token();

  CancellationSource moved(std::move(original));

  EXPECT_FALSE(original.token().is_cancelled());
  EXPECT_FALSE(moved.token().is_cancelled());
  original.cancel();
  EXPECT_TRUE(original.token().is_cancelled());
  EXPECT_FALSE(original_token.is_cancelled());

  moved.cancel();
  EXPECT_TRUE(moved.token().is_cancelled());
  EXPECT_TRUE(original_token.is_cancelled());
}

TEST(CancellationTest, SourceRemainsUsableAfterMoveAssignment) {
  CancellationSource original;
  const CancellationToken original_token = original.token();
  CancellationSource assigned;
  assigned.cancel();
  const CancellationToken assigned_token = assigned.token();

  assigned = std::move(original);

  EXPECT_FALSE(original.token().is_cancelled());
  EXPECT_FALSE(assigned.token().is_cancelled());
  original.cancel();
  assigned.cancel();
  EXPECT_TRUE(original.token().is_cancelled());
  EXPECT_TRUE(assigned.token().is_cancelled());
  EXPECT_TRUE(original_token.is_cancelled());
  EXPECT_TRUE(assigned_token.is_cancelled());
}

TEST(ImageFrameTest, RejectsAllocationThatExceedsBudget) {
  const auto result =
      ImageFrame::allocate_bgra8(1'000'000, 1'000'000, 256_MiB);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ErrorCode::resource_limit);
}

TEST(ImageFrameTest, AllocatesBgra8PixelsWithinBudget) {
  const auto result = ImageFrame::allocate_bgra8(2, 3, 1_MiB);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().stride, 8U);
  EXPECT_EQ(result.value().pixels.size(), 24U);
}

TEST(ImageFrameTest, UsesWindowsCompatibleDimensionAndStrideTypes) {
  EXPECT_TRUE((std::is_same_v<decltype(ImageFrame::width), std::uint32_t>));
  EXPECT_TRUE((std::is_same_v<decltype(ImageFrame::height), std::uint32_t>));
  EXPECT_TRUE((std::is_same_v<decltype(ImageFrame::stride), std::uint32_t>));
}

TEST(ImageFrameTest, RejectsStrideThatExceedsUint32) {
  constexpr std::uint32_t width =
      std::numeric_limits<std::uint32_t>::max() / 4U + 1U;

  const auto result = ImageFrame::allocate_bgra8(
      width, 1, std::numeric_limits<std::size_t>::max());

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ErrorCode::resource_limit);
}

}  // namespace
}  // namespace viewer::core
