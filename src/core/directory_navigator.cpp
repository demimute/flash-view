#include "viewer/core/directory_navigator.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <optional>
#include <span>
#include <system_error>
#include <utility>

#include "viewer/core/format_probe.h"
#include "viewer/core/natural_sort.h"

namespace viewer::core {
namespace {

constexpr std::size_t header_size = 16;

[[nodiscard]] std::optional<ImageFormat> probe_file(
    const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return std::nullopt;
  }

  std::array<std::byte, header_size> header{};
  input.read(reinterpret_cast<char*>(header.data()),
             static_cast<std::streamsize>(header.size()));
  const auto bytes_read = input.gcount();
  if (input.bad()) {
    return std::nullopt;
  }

  return probe_format(
      std::span<const std::byte>(header.data(),
                                 static_cast<std::size_t>(bytes_read)));
}

[[nodiscard]] std::filesystem::path normalized(
    const std::filesystem::path& path) {
  return path.lexically_normal();
}

[[nodiscard]] Error io_error(std::wstring message) {
  return Error{
      .code = ErrorCode::io_error,
      .message = std::move(message),
  };
}

[[nodiscard]] Error unsupported_format(std::wstring message) {
  return Error{
      .code = ErrorCode::unsupported_format,
      .message = std::move(message),
  };
}

}  // namespace

Result<DirectoryNavigator> DirectoryNavigator::scan(
    const std::filesystem::path& selected) {
  std::error_code error;
  const auto selected_status = std::filesystem::status(selected, error);
  if (error || !std::filesystem::is_regular_file(selected_status)) {
    return Result<DirectoryNavigator>::failure(
        io_error(L"Selected image is not a readable regular file"));
  }

  const auto selected_format = probe_file(selected);
  if (!selected_format.has_value()) {
    return Result<DirectoryNavigator>::failure(
        io_error(L"Could not read the selected image"));
  }
  if (*selected_format == ImageFormat::unknown) {
    return Result<DirectoryNavigator>::failure(
        unsupported_format(L"Selected file is not a supported image"));
  }

  auto directory = selected.parent_path();
  if (directory.empty()) {
    directory = std::filesystem::path{"."};
  }

  std::vector<std::filesystem::path> items;
  std::filesystem::directory_iterator iterator(directory, error);
  const std::filesystem::directory_iterator end;
  if (error) {
    return Result<DirectoryNavigator>::failure(
        io_error(L"Could not read the selected image directory"));
  }

  while (iterator != end) {
    const auto& entry = *iterator;
    const bool is_regular_file = entry.is_regular_file(error);
    if (error) {
      return Result<DirectoryNavigator>::failure(
          io_error(L"Could not inspect an image directory entry"));
    }

    if (is_regular_file) {
      const auto format = probe_file(entry.path());
      if (format.has_value() && *format != ImageFormat::unknown) {
        items.push_back(entry.path());
      }
    }

    iterator.increment(error);
    if (error) {
      return Result<DirectoryNavigator>::failure(
          io_error(L"Could not continue reading the image directory"));
    }
  }

  const NaturalLess natural_less;
  std::sort(items.begin(), items.end(),
            [&natural_less](const auto& lhs, const auto& rhs) {
              const auto lhs_name = lhs.filename().wstring();
              const auto rhs_name = rhs.filename().wstring();
              if (natural_less(lhs_name, rhs_name)) {
                return true;
              }
              if (natural_less(rhs_name, lhs_name)) {
                return false;
              }
              return lhs_name < rhs_name;
            });

  const auto selected_normalized = normalized(selected);
  auto current = items.end();
  bool comparison_failed = false;
  for (auto candidate = items.begin(); candidate != items.end(); ++candidate) {
    if (normalized(*candidate) == selected_normalized) {
      current = candidate;
      break;
    }

    std::error_code equivalent_error;
    const bool is_selected =
        std::filesystem::equivalent(*candidate, selected, equivalent_error);
    if (equivalent_error) {
      comparison_failed = true;
      continue;
    }
    if (is_selected) {
      current = candidate;
      break;
    }
  }

  if (current == items.end()) {
    if (comparison_failed) {
      return Result<DirectoryNavigator>::failure(
          io_error(L"Could not compare the selected image with directory "
                   L"entries"));
    }
    return Result<DirectoryNavigator>::failure(
        io_error(L"Selected image was not found in its directory"));
  }

  const auto current_index =
      static_cast<std::size_t>(std::distance(items.begin(), current));
  return Result<DirectoryNavigator>::success(DirectoryNavigator(
      std::move(items), current_index));
}

const std::filesystem::path& DirectoryNavigator::previous() noexcept {
  current_index_ =
      current_index_ == 0 ? items_.size() - 1 : current_index_ - 1;
  return current();
}

const std::filesystem::path& DirectoryNavigator::next() noexcept {
  ++current_index_;
  if (current_index_ == items_.size()) {
    current_index_ = 0;
  }
  return current();
}

}  // namespace viewer::core
