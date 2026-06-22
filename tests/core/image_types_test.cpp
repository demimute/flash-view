#include <gtest/gtest.h>

#include "viewer/core/cancellation.h"
#include "viewer/core/image_frame.h"
#include "viewer/core/load_request.h"

namespace viewer::core {
namespace {

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

}  // namespace
}  // namespace viewer::core
