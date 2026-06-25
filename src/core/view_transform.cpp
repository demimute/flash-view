#include "viewer/core/view_transform.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace viewer::core {
namespace {

constexpr float default_minimum_scale = 0.01F;
constexpr float maximum_scale = 64.0F;

}  // namespace

void ViewTransform::fit(SizeU image, SizeU viewport) noexcept {
  offset_x_ = 0.0F;
  offset_y_ = 0.0F;

  if (image.width == 0 || image.height == 0 || viewport.width == 0 ||
      viewport.height == 0) {
    scale_ = 1.0F;
    minimum_scale_ = default_minimum_scale;
    return;
  }

  if (rotation_ == Rotation::degrees_90 ||
      rotation_ == Rotation::degrees_270) {
    std::swap(image.width, image.height);
  }

  const double horizontal_scale =
      static_cast<double>(viewport.width) / image.width;
  const double vertical_scale =
      static_cast<double>(viewport.height) / image.height;
  scale_ = std::min(
      static_cast<float>(std::min(horizontal_scale, vertical_scale)),
      maximum_scale);
  minimum_scale_ = std::min(default_minimum_scale, scale_);
}

void ViewTransform::one_to_one() noexcept {
  scale_ = 1.0F;
  minimum_scale_ = default_minimum_scale;
  offset_x_ = 0.0F;
  offset_y_ = 0.0F;
}

void ViewTransform::zoom_by(float factor) noexcept {
  if (!std::isfinite(factor) || factor <= 0.0F) {
    return;
  }

  scale_ = std::clamp(scale_ * factor, minimum_scale_, maximum_scale);
}

void ViewTransform::pan_by(float dx, float dy) noexcept {
  if (!std::isfinite(dx) || !std::isfinite(dy)) {
    return;
  }

  const float next_x = offset_x_ + dx;
  const float next_y = offset_y_ + dy;
  if (!std::isfinite(next_x) || !std::isfinite(next_y)) {
    return;
  }

  offset_x_ = next_x;
  offset_y_ = next_y;
}

void ViewTransform::rotate_clockwise() noexcept {
  switch (rotation_) {
    case Rotation::degrees_0:
      rotation_ = Rotation::degrees_90;
      break;
    case Rotation::degrees_90:
      rotation_ = Rotation::degrees_180;
      break;
    case Rotation::degrees_180:
      rotation_ = Rotation::degrees_270;
      break;
    case Rotation::degrees_270:
      rotation_ = Rotation::degrees_0;
      break;
  }
}

}  // namespace viewer::core
