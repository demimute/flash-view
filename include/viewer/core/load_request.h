#pragma once

#include <cstdint>
#include <filesystem>

namespace viewer::core {

struct LoadRequest {
  std::filesystem::path path;
  std::uint64_t generation = 0;
};

}  // namespace viewer::core
