#pragma once

#include <Windows.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "viewer/core/image_frame.h"
#include "viewer/core/result.h"
#include "viewer/core/view_transform.h"

namespace viewer::render {

struct ToolbarOverlayItem {
  RECT bounds{};
  std::size_t icon = 0;
};

struct ThumbnailOverlayItem {
  RECT cell{};
  RECT image_bounds{};
  const core::ImageFrame* frame = nullptr;
  std::wstring label;
  bool selected = false;
};

struct RenderOverlay {
  bool image_viewport_visible = false;
  RECT image_viewport{};
  bool toolbar_visible = false;
  RECT toolbar_bounds{};
  std::vector<ToolbarOverlayItem> toolbar_items;
  bool thumbnails_visible = false;
  RECT thumbnail_panel{};
  RECT thumbnail_splitter{};
  bool thumbnail_scrollbar_visible = false;
  RECT thumbnail_scrollbar_track{};
  RECT thumbnail_scrollbar_thumb{};
  std::vector<ThumbnailOverlayItem> thumbnails;
};

class D3dRenderer {
 public:
  D3dRenderer();
  ~D3dRenderer();
  D3dRenderer(D3dRenderer&&) noexcept;
  D3dRenderer& operator=(D3dRenderer&&) noexcept;
  D3dRenderer(const D3dRenderer&) = delete;
  D3dRenderer& operator=(const D3dRenderer&) = delete;

  core::Result<bool> initialize(HWND window);
  core::Result<bool> resize(unsigned width, unsigned height);
  core::Result<bool> set_image(const core::ImageFrame& frame);
  void set_status_text(std::wstring text);
  core::Result<bool> draw(const core::ViewTransform& transform,
                          const RenderOverlay* overlay = nullptr);
  void clear_image();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace viewer::render
