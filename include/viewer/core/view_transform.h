#pragma once

#include <cstdint>

namespace viewer::core {

struct SizeU {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

enum class Rotation {
  degrees_0,
  degrees_90,
  degrees_180,
  degrees_270,
};

class ViewTransform {
 public:
  void fit(SizeU image, SizeU viewport) noexcept;
  void one_to_one() noexcept;
  void zoom_by(float factor) noexcept;
  void pan_by(float dx, float dy) noexcept;
  void rotate_clockwise() noexcept;

  [[nodiscard]] float scale() const noexcept { return scale_; }

  // Offsets are additional screen-pixel translations. The renderer owns
  // automatic centering of the transformed image rectangle.
  [[nodiscard]] float offset_x() const noexcept { return offset_x_; }
  [[nodiscard]] float offset_y() const noexcept { return offset_y_; }
  [[nodiscard]] Rotation rotation() const noexcept { return rotation_; }

 private:
  float scale_ = 1.0F;
  float offset_x_ = 0.0F;
  float offset_y_ = 0.0F;
  Rotation rotation_ = Rotation::degrees_0;
};

}  // namespace viewer::core
