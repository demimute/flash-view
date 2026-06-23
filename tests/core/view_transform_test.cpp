#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "viewer/core/view_transform.h"

namespace viewer::core {
namespace {

TEST(ViewTransformTest, UsesUint32Dimensions) {
  EXPECT_TRUE((std::is_same_v<decltype(SizeU::width), std::uint32_t>));
  EXPECT_TRUE((std::is_same_v<decltype(SizeU::height), std::uint32_t>));
}

TEST(ViewTransformTest, FitsImageInsideViewportWithoutAutomaticCentering) {
  ViewTransform transform;
  transform.pan_by(12.0F, -8.0F);

  transform.fit({4000, 2000}, {1000, 1000});

  EXPECT_FLOAT_EQ(transform.scale(), 0.25F);
  EXPECT_FLOAT_EQ(transform.offset_x(), 0.0F);
  EXPECT_FLOAT_EQ(transform.offset_y(), 0.0F);
}

TEST(ViewTransformTest, OneToOneResetsScaleAndOffsets) {
  ViewTransform transform;
  transform.zoom_by(2.0F);
  transform.pan_by(12.0F, -8.0F);

  transform.one_to_one();

  EXPECT_FLOAT_EQ(transform.scale(), 1.0F);
  EXPECT_FLOAT_EQ(transform.offset_x(), 0.0F);
  EXPECT_FLOAT_EQ(transform.offset_y(), 0.0F);
}

TEST(ViewTransformTest, ZoomIsClamped) {
  ViewTransform transform;

  transform.zoom_by(1000.0F);
  EXPECT_FLOAT_EQ(transform.scale(), 64.0F);

  transform.zoom_by(0.00001F);
  EXPECT_FLOAT_EQ(transform.scale(), 0.01F);
}

TEST(ViewTransformTest, ZoomFromSmallFitUsesFittedScaleAsMinimum) {
  ViewTransform transform;
  transform.fit({100000, 100000}, {800, 800});
  ASSERT_FLOAT_EQ(transform.scale(), 0.008F);

  transform.zoom_by(1.1F);
  EXPECT_NEAR(transform.scale(), 0.0088F, 0.000001F);

  transform.zoom_by(0.00001F);
  EXPECT_FLOAT_EQ(transform.scale(), 0.008F);
}

TEST(ViewTransformTest, OneToOneRestoresDefaultMinimumScale) {
  ViewTransform transform;
  transform.fit({100000, 100000}, {800, 800});

  transform.one_to_one();
  transform.zoom_by(0.00001F);

  EXPECT_FLOAT_EQ(transform.scale(), 0.01F);
}

TEST(ViewTransformTest, RotationCyclesClockwise) {
  ViewTransform transform;

  transform.rotate_clockwise();
  EXPECT_EQ(transform.rotation(), Rotation::degrees_90);
  transform.rotate_clockwise();
  EXPECT_EQ(transform.rotation(), Rotation::degrees_180);
  transform.rotate_clockwise();
  EXPECT_EQ(transform.rotation(), Rotation::degrees_270);
  transform.rotate_clockwise();
  EXPECT_EQ(transform.rotation(), Rotation::degrees_0);
}

TEST(ViewTransformTest, ZeroSizedFitResetsScaleAndOffsets) {
  ViewTransform transform;
  transform.zoom_by(2.0F);
  transform.pan_by(12.0F, -8.0F);

  transform.fit({0, 2000}, {1000, 1000});

  EXPECT_FLOAT_EQ(transform.scale(), 1.0F);
  EXPECT_FLOAT_EQ(transform.offset_x(), 0.0F);
  EXPECT_FLOAT_EQ(transform.offset_y(), 0.0F);
}

TEST(ViewTransformTest, ZeroSizedFitRestoresDefaultMinimumScale) {
  ViewTransform transform;
  transform.fit({100000, 100000}, {800, 800});

  transform.fit({0, 2000}, {1000, 1000});
  transform.zoom_by(0.00001F);

  EXPECT_FLOAT_EQ(transform.scale(), 0.01F);
}

TEST(ViewTransformTest, FitUsesRotatedDimensionsAtNinetyDegrees) {
  ViewTransform transform;
  transform.rotate_clockwise();

  transform.fit({4000, 2000}, {1000, 500});

  EXPECT_FLOAT_EQ(transform.scale(), 0.125F);
}

TEST(ViewTransformTest, FitUsesRotatedDimensionsAtTwoHundredSeventyDegrees) {
  ViewTransform transform;
  transform.rotate_clockwise();
  transform.rotate_clockwise();
  transform.rotate_clockwise();

  transform.fit({4000, 2000}, {1000, 500});

  EXPECT_FLOAT_EQ(transform.scale(), 0.125F);
}

TEST(ViewTransformTest, InvalidZoomFactorsLeaveStateUnchanged) {
  ViewTransform transform;
  transform.zoom_by(2.0F);
  const float expected = transform.scale();

  transform.zoom_by(std::numeric_limits<float>::quiet_NaN());
  EXPECT_FLOAT_EQ(transform.scale(), expected);
  transform.zoom_by(std::numeric_limits<float>::infinity());
  EXPECT_FLOAT_EQ(transform.scale(), expected);
  transform.zoom_by(-1.0F);
  EXPECT_FLOAT_EQ(transform.scale(), expected);
}

TEST(ViewTransformTest, InvalidPanDeltasLeaveBothOffsetsUnchanged) {
  ViewTransform transform;
  transform.pan_by(12.0F, -8.0F);

  transform.pan_by(std::numeric_limits<float>::quiet_NaN(), 1.0F);
  EXPECT_FLOAT_EQ(transform.offset_x(), 12.0F);
  EXPECT_FLOAT_EQ(transform.offset_y(), -8.0F);
  transform.pan_by(1.0F, std::numeric_limits<float>::infinity());
  EXPECT_FLOAT_EQ(transform.offset_x(), 12.0F);
  EXPECT_FLOAT_EQ(transform.offset_y(), -8.0F);
}

TEST(ViewTransformTest, ExtremeDimensionsProduceFiniteScale) {
  ViewTransform transform;
  constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();

  transform.fit({maximum, 1}, {1, maximum});

  EXPECT_TRUE(std::isfinite(transform.scale()));
  EXPECT_GT(transform.scale(), 0.0F);
}

TEST(ViewTransformTest, RotationDoesNotRefitOrResetPan) {
  ViewTransform transform;
  transform.zoom_by(2.0F);
  transform.pan_by(12.0F, -8.0F);

  transform.rotate_clockwise();

  EXPECT_FLOAT_EQ(transform.scale(), 2.0F);
  EXPECT_FLOAT_EQ(transform.offset_x(), 12.0F);
  EXPECT_FLOAT_EQ(transform.offset_y(), -8.0F);
}

}  // namespace
}  // namespace viewer::core
