#pragma once

#include <cstddef>
#include <filesystem>
#include <utility>
#include <vector>

#include "viewer/core/result.h"

namespace viewer::core {

class DirectoryNavigator {
 public:
  [[nodiscard]] static Result<DirectoryNavigator> scan(
      const std::filesystem::path& selected);

  [[nodiscard]] const std::vector<std::filesystem::path>& items()
      const noexcept {
    return items_;
  }

  [[nodiscard]] std::size_t current_index() const noexcept {
    return current_index_;
  }

  [[nodiscard]] const std::filesystem::path& current() const noexcept {
    return items_[current_index_];
  }

  [[nodiscard]] const std::filesystem::path& previous() noexcept;
  [[nodiscard]] const std::filesystem::path& next() noexcept;
  [[nodiscard]] const std::filesystem::path& select(
      std::size_t index) noexcept;

 private:
  DirectoryNavigator(std::vector<std::filesystem::path> items,
                     std::size_t current_index)
      : items_(std::move(items)), current_index_(current_index) {}

  std::vector<std::filesystem::path> items_;
  std::size_t current_index_;
};

}  // namespace viewer::core
