#include "app/main_window.h"

#include <optional>
#include <utility>

#include "viewer/app/window_state.h"
#include "viewer/core/result.h"
#include "viewer/core/view_transform.h"
#include "viewer/render/d3d_renderer.h"

namespace viewer::app {
namespace {

constexpr wchar_t window_class_name[] = L"FastImageViewer.MainWindow";
constexpr wchar_t window_title[] = L"Fast Image Viewer";

}  // namespace

struct MainWindow::Impl {
  HWND window = nullptr;
  bool renderer_ready = false;
  render::D3dRenderer renderer;
  std::optional<std::filesystem::path> pending_path;

  void report_renderer_failure(const core::Error& error) {
    renderer_ready = false;
    MessageBoxW(window, error.message.c_str(), window_title,
                MB_OK | MB_ICONERROR);
    PostQuitMessage(1);
  }
};

MainWindow::MainWindow() : impl_(std::make_unique<Impl>()) {}

MainWindow::~MainWindow() {
  if (impl_->window != nullptr && IsWindow(impl_->window)) {
    DestroyWindow(impl_->window);
  }
}

bool MainWindow::create(HINSTANCE instance, int show_command) {
  WNDCLASSEXW window_class{
      .cbSize = sizeof(WNDCLASSEXW),
      .style = CS_HREDRAW | CS_VREDRAW,
      .lpfnWndProc = &MainWindow::window_proc,
      .hInstance = instance,
      .hCursor = LoadCursorW(nullptr, IDC_ARROW),
      .hbrBackground = nullptr,
      .lpszClassName = window_class_name,
  };
  if (RegisterClassExW(&window_class) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  const HWND window = CreateWindowExW(
      0, window_class_name, window_title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
      CW_USEDEFAULT, 1200, 800, nullptr, nullptr, instance, this);
  if (window == nullptr) {
    return false;
  }

  auto initialize_result = impl_->renderer.initialize(window);
  if (!initialize_result.has_value()) {
    DestroyWindow(window);
    return false;
  }
  impl_->renderer_ready = true;

  ShowWindow(window, show_command);
  UpdateWindow(window);
  return true;
}

void MainWindow::open_path(const std::filesystem::path& path) {
  impl_->pending_path = path;
}

LRESULT CALLBACK MainWindow::window_proc(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  MainWindow* self = nullptr;
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    self = static_cast<MainWindow*>(create->lpCreateParams);
    self->impl_->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(self));
  } else {
    self = reinterpret_cast<MainWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
  }

  const LRESULT result =
      self != nullptr
          ? self->handle_message(message, wparam, lparam)
          : DefWindowProcW(window, message, wparam, lparam);

  if (message == WM_NCDESTROY) {
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    if (self != nullptr && self->impl_->window == window) {
      self->impl_->window = nullptr;
    }
  }
  return result;
}

LRESULT MainWindow::handle_message(
    UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_SIZE: {
      const unsigned width = LOWORD(lparam);
      const unsigned height = HIWORD(lparam);
      const bool minimized = wparam == SIZE_MINIMIZED;
      if (!impl_->renderer_ready ||
          !WindowState::should_resize(minimized, width, height)) {
        return 0;
      }

      auto resize_result = impl_->renderer.resize(width, height);
      if (!resize_result.has_value()) {
        impl_->report_renderer_failure(resize_result.error());
      }
      return 0;
    }

    case WM_PAINT: {
      PAINTSTRUCT paint{};
      BeginPaint(impl_->window, &paint);

      std::optional<core::Error> draw_error;
      if (impl_->renderer_ready) {
        auto draw_result = impl_->renderer.draw(core::ViewTransform{});
        if (!draw_result.has_value()) {
          draw_error = std::move(draw_result.error());
        }
      }

      EndPaint(impl_->window, &paint);
      if (draw_error.has_value()) {
        impl_->report_renderer_failure(*draw_error);
      }
      return 0;
    }

    case WM_ERASEBKGND:
      return 1;

    case WM_DPICHANGED: {
      const auto* suggested = reinterpret_cast<const RECT*>(lparam);
      SetWindowPos(impl_->window, nullptr, suggested->left, suggested->top,
                   suggested->right - suggested->left,
                   suggested->bottom - suggested->top,
                   SWP_NOACTIVATE | SWP_NOZORDER);
      return 0;
    }

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;

    default:
      return DefWindowProcW(impl_->window, message, wparam, lparam);
  }
}

}  // namespace viewer::app
