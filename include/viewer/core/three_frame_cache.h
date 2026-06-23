#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "viewer/core/image_frame.h"

namespace viewer::core {

enum class LoadedImagePurpose : std::uint8_t {
  display,
  prefetch,
};

struct CachedFrame {
  std::filesystem::path path;
  std::shared_ptr<ImageFrame> frame;
};

[[nodiscard]] inline std::filesystem::path weakly_normalized(
    const std::filesystem::path& path) {
  std::error_code error;
  auto normalized = std::filesystem::weakly_canonical(path, error);
  if (!error) {
    return normalized.lexically_normal();
  }
  return path.lexically_normal();
}

[[nodiscard]] inline bool paths_match(
    const std::filesystem::path& lhs,
    const std::filesystem::path& rhs) {
  if (weakly_normalized(lhs) == weakly_normalized(rhs)) {
    return true;
  }

  std::error_code error;
  const bool equivalent = std::filesystem::equivalent(lhs, rhs, error);
  return !error && equivalent;
}

class ThreeFrameCache {
 public:
  [[nodiscard]] std::shared_ptr<ImageFrame> find(
      const std::filesystem::path& path) const {
    for (const auto& entry : frames_) {
      if (entry.has_value() && entry->frame &&
          paths_match(entry->path, path)) {
        return entry->frame;
      }
    }
    return nullptr;
  }

  void remember(std::filesystem::path path,
                std::shared_ptr<ImageFrame> frame) {
    if (!frame) {
      return;
    }

    for (auto& entry : frames_) {
      if (entry.has_value() && paths_match(entry->path, path)) {
        entry = CachedFrame{std::move(path), std::move(frame)};
        return;
      }
    }

    for (auto& entry : frames_) {
      if (!entry.has_value()) {
        entry = CachedFrame{std::move(path), std::move(frame)};
        return;
      }
    }

    frames_[0] = CachedFrame{std::move(path), std::move(frame)};
  }

  void retain(const std::filesystem::path& current,
              const std::filesystem::path& previous,
              const std::filesystem::path& next) {
    for (auto& entry : frames_) {
      if (entry.has_value() &&
          !paths_match(entry->path, current) &&
          !paths_match(entry->path, previous) &&
          !paths_match(entry->path, next)) {
        entry.reset();
      }
    }
  }

 private:
  std::array<std::optional<CachedFrame>, 3> frames_{};
};

class PrefetchTracker {
 public:
  [[nodiscard]] bool contains(
      const std::filesystem::path& path) const {
    for (const auto& in_flight_path : in_flight_) {
      if (paths_match(in_flight_path, path)) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool should_submit(
      const std::filesystem::path& path,
      const ThreeFrameCache& cache) {
    if (cache.find(path) || contains(path)) {
      return false;
    }
    in_flight_.push_back(path);
    return true;
  }

  void finish(const std::filesystem::path& path) {
    for (auto iterator = in_flight_.begin();
         iterator != in_flight_.end(); ++iterator) {
      if (paths_match(*iterator, path)) {
        in_flight_.erase(iterator);
        return;
      }
    }
  }

  void clear() { in_flight_.clear(); }

 private:
  std::vector<std::filesystem::path> in_flight_;
};

[[nodiscard]] inline bool is_three_frame_relevant(
    const std::filesystem::path& path,
    const std::filesystem::path& current,
    const std::filesystem::path& previous,
    const std::filesystem::path& next) {
  return paths_match(path, current) || paths_match(path, previous) ||
         paths_match(path, next);
}

[[nodiscard]] inline bool should_accept_loaded_image(
    LoadedImagePurpose purpose,
    const std::filesystem::path& loaded_path,
    const std::filesystem::path& current,
    const std::filesystem::path& previous,
    const std::filesystem::path& next,
    std::uint64_t request_generation,
    std::uint64_t current_generation) {
  if (purpose == LoadedImagePurpose::display) {
    return request_generation == current_generation &&
           paths_match(loaded_path, current);
  }
  return is_three_frame_relevant(loaded_path, current, previous, next);
}

}  // namespace viewer::core
