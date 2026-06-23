#include <gtest/gtest.h>

#include "viewer/render/render_math.h"

namespace viewer::render {
namespace {

TEST(RenderMathTest, CentersScaledImageInViewport) {
  const auto matrix = make_image_transform(
      {4000, 2000}, {1000, 1000}, 0.25F, core::Rotation::degrees_0,
      {0.0F, 0.0F});

  const auto center = transform_point(matrix, {2000.0F, 1000.0F});
  EXPECT_FLOAT_EQ(center.x, 500.0F);
  EXPECT_FLOAT_EQ(center.y, 500.0F);
}

TEST(RenderMathTest, KeepsRotatedImageCenterAtViewportCenter) {
  const auto matrix = make_image_transform(
      {4000, 2000}, {1000, 1000}, 0.25F, core::Rotation::degrees_90,
      {0.0F, 0.0F});

  const auto center = transform_point(matrix, {2000.0F, 1000.0F});
  EXPECT_NEAR(center.x, 500.0F, 0.0001F);
  EXPECT_NEAR(center.y, 500.0F, 0.0001F);

  const auto top_left = transform_point(matrix, {0.0F, 0.0F});
  EXPECT_NEAR(top_left.x, 750.0F, 0.0001F);
  EXPECT_NEAR(top_left.y, 0.0F, 0.0001F);
}

TEST(RenderMathTest, AppliesOffsetAfterAutomaticCentering) {
  const auto matrix = make_image_transform(
      {4000, 2000}, {1000, 1000}, 0.25F, core::Rotation::degrees_270,
      {35.0F, -20.0F});

  const auto center = transform_point(matrix, {2000.0F, 1000.0F});
  EXPECT_NEAR(center.x, 535.0F, 0.0001F);
  EXPECT_NEAR(center.y, 480.0F, 0.0001F);
}

}  // namespace
}  // namespace viewer::render
