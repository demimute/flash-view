#pragma once

#include "viewer/core/view_transform.h"

namespace viewer::render {

struct PointF {
  float x = 0.0F;
  float y = 0.0F;
};

// D2D-compatible row-vector affine matrix:
// x' = x * m11 + y * m21 + dx
// y' = x * m12 + y * m22 + dy
struct Affine2D {
  float m11 = 1.0F;
  float m12 = 0.0F;
  float m21 = 0.0F;
  float m22 = 1.0F;
  float dx = 0.0F;
  float dy = 0.0F;
};

[[nodiscard]] constexpr PointF transform_point(Affine2D matrix,
                                               PointF point) noexcept {
  return {
      point.x * matrix.m11 + point.y * matrix.m21 + matrix.dx,
      point.x * matrix.m12 + point.y * matrix.m22 + matrix.dy,
  };
}

[[nodiscard]] constexpr Affine2D make_image_transform(
    core::SizeU image, core::SizeU viewport, float scale,
    core::Rotation rotation, PointF offset) noexcept {
  float cosine = 1.0F;
  float sine = 0.0F;
  switch (rotation) {
    case core::Rotation::degrees_0:
      break;
    case core::Rotation::degrees_90:
      cosine = 0.0F;
      sine = 1.0F;
      break;
    case core::Rotation::degrees_180:
      cosine = -1.0F;
      sine = 0.0F;
      break;
    case core::Rotation::degrees_270:
      cosine = 0.0F;
      sine = -1.0F;
      break;
  }

  Affine2D matrix{
      .m11 = scale * cosine,
      .m12 = scale * sine,
      .m21 = -scale * sine,
      .m22 = scale * cosine,
  };

  const PointF image_center{
      static_cast<float>(image.width) / 2.0F,
      static_cast<float>(image.height) / 2.0F,
  };
  const PointF transformed_center = transform_point(matrix, image_center);
  matrix.dx = static_cast<float>(viewport.width) / 2.0F + offset.x -
              transformed_center.x;
  matrix.dy = static_cast<float>(viewport.height) / 2.0F + offset.y -
              transformed_center.y;
  return matrix;
}

}  // namespace viewer::render
