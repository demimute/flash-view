#pragma once

#include <Windows.h>

#include <memory>
#include <string>

#include "viewer/core/image_frame.h"
#include "viewer/core/result.h"
#include "viewer/core/view_transform.h"

namespace viewer::render {

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
  core::Result<bool> draw(const core::ViewTransform& transform);
  void clear_image();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace viewer::render
