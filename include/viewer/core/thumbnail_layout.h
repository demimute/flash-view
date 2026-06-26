#pragma once

#include <algorithm>
#include <cstdint>

namespace viewer::core {

enum class ThumbnailDock {
  top,
  left,
  right,
  bottom,
};

struct ThumbnailLayoutState {
  bool visible = false;
  bool preview_visible = false;
  ThumbnailDock dock = ThumbnailDock::bottom;
  std::uint32_t thumbnail_size = 128;
  std::uint32_t panel_extent = 220;
  std::uint32_t preview_extent = 260;

  void toggle_visible() noexcept { visible = !visible; }
  void toggle_preview() noexcept { preview_visible = !preview_visible; }

  void grow_thumbnails() noexcept {
    thumbnail_size = std::min<std::uint32_t>(thumbnail_size + 16, 320);
  }

  void shrink_thumbnails() noexcept {
    thumbnail_size = thumbnail_size <= 48 ? 48 : thumbnail_size - 16;
  }

  void resize_panel(std::uint32_t extent) noexcept {
    panel_extent = std::clamp<std::uint32_t>(extent, 96, 520);
  }

  void cycle_dock() noexcept {
    switch (dock) {
      case ThumbnailDock::bottom:
        dock = ThumbnailDock::left;
        break;
      case ThumbnailDock::left:
        dock = ThumbnailDock::right;
        break;
      case ThumbnailDock::right:
        dock = ThumbnailDock::top;
        break;
      case ThumbnailDock::top:
        dock = ThumbnailDock::bottom;
        break;
    }
  }
};

}  // namespace viewer::core
