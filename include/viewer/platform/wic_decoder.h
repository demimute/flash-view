#pragma once

#include <cstddef>
#include <filesystem>

#include "viewer/core/image_frame.h"
#include "viewer/core/result.h"

namespace viewer::platform {

class WicDecoder {
 public:
  [[nodiscard]] core::Result<core::ImageFrame> decode(
      const std::filesystem::path& path,
      std::size_t byte_budget) const;
};

}  // namespace viewer::platform
