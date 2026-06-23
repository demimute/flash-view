#include "app/main_window.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

#include "viewer/app/window_state.h"
#include "viewer/core/async_shutdown.h"
#include "viewer/core/cancellation.h"
#include "viewer/core/image_frame.h"
#include "viewer/core/load_generation.h"
#include "viewer/core/priority_executor.h"
#include "viewer/core/result.h"
#include "viewer/core/view_transform.h"
#include "viewer/platform/wic_decoder.h"
#include "viewer/render/d3d_renderer.h"

namespace viewer::app {
namespace {

constexpr wchar_t window_class_name[] = L"FastImageViewer.MainWindow";
constexpr wchar_t window_title[] = L"Fast Image Viewer";
constexpr UINT image_ready_message = WM_APP + 1;
constexpr std::size_t decode_byte_budget =
    std::size_t{512} * std::size_t{1024} * std::size_t{1024};

std::uint64_t next_instance_token() noexcept {
  static std::atomic_uint64_t next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

std::size_t worker_thread_count() noexcept {
  const unsigned hardware_threads = std::thread::hardware_concurrency();
  return hardware_threads > 1 ? hardware_threads - 1 : 1;
}

struct LoadedImage {
  std::uint64_t instance_token;
  std::uint64_t generation;
  std::filesystem::path path;
  core::Result<core::ImageFrame> result;
};

struct AsyncLoadContext {
  explicit AsyncLoadContext(std::uint64_t instance_token)
      : executor(worker_thread_count()),
        completions(nullptr, instance_token, false) {}

  core::LoadGeneration generation;
  core::CancellationSource cancellation;
  core::PriorityExecutor executor;
  core::CompletionGate<HWND, std::uint64_t> completions;
};

void reap_async_load_context(
    std::shared_ptr<AsyncLoadContext> context) {
  std::thread([context = std::move(context)] {
    context->executor.stop();
  }).detach();
}

}  // namespace

struct MainWindow::Impl {
  HWND window = nullptr;
  bool renderer_ready = false;
  const std::uint64_t instance_token = next_instance_token();
  core::AsyncShutdownState<AsyncLoadContext> async_load{
      std::make_shared<AsyncLoadContext>(instance_token)};
  render::D3dRenderer renderer;
  core::ViewTransform transform;

  void report_renderer_failure(const core::Error& error) {
    renderer_ready = false;
    MessageBoxW(window, error.message.c_str(), window_title,
                MB_OK | MB_ICONERROR);
    PostQuitMessage(1);
  }

  void discard_pending_completions() noexcept {
    if (window == nullptr) {
      return;
    }

    MSG message{};
    while (PeekMessageW(&message, window, image_ready_message,
                        image_ready_message, PM_REMOVE)) {
      delete reinterpret_cast<LoadedImage*>(message.lParam);
    }
  }

  void shutdown_async() {
    std::shared_ptr<AsyncLoadContext> context =
        async_load.take_context();
    if (!context) {
      return;
    }

    static_cast<void>(context->completions.close());
    context->cancellation.cancel();
    discard_pending_completions();
    reap_async_load_context(std::move(context));
  }
};

MainWindow::MainWindow() : impl_(std::make_unique<Impl>()) {}

MainWindow::~MainWindow() {
  impl_->shutdown_async();
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

  if (const auto context = impl_->async_load.context()) {
    context->completions.activate(window, impl_->instance_token);
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
  const std::shared_ptr<AsyncLoadContext> context =
      impl_->async_load.context();
  if (impl_->window == nullptr || !context) {
    return;
  }

  context->cancellation.cancel();
  context->cancellation = core::CancellationSource{};

  const core::CancellationToken cancellation =
      context->cancellation.token();
  const std::uint64_t generation = context->generation.begin();
  const std::uint64_t instance_token = impl_->instance_token;

  const bool submitted = context->executor.submit(
      core::Priority::current_image,
      [context, instance_token, generation, cancellation, path] {
        if (cancellation.is_cancelled() ||
            !context->generation.is_current(generation)) {
          return;
        }

        const platform::WicDecoder decoder;
        auto decoded = decoder.decode(path, decode_byte_budget);
        if (cancellation.is_cancelled() ||
            !context->generation.is_current(generation)) {
          return;
        }

        auto payload = std::make_unique<LoadedImage>(LoadedImage{
            .instance_token = instance_token,
            .generation = generation,
            .path = path,
            .result = std::move(decoded),
        });
        static_cast<void>(context->completions.publish(
            payload,
            [](HWND target, std::uint64_t gate_token,
               LoadedImage* loaded) {
              if (loaded->instance_token != gate_token) {
                return false;
              }
              return PostMessageW(
                         target, image_ready_message, 0,
                         reinterpret_cast<LPARAM>(loaded)) != FALSE;
            }));
      });

  if (!submitted) {
    context->cancellation.cancel();
    MessageBoxW(impl_->window, L"Unable to schedule image decoding.",
                window_title, MB_OK | MB_ICONERROR);
  }
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
        auto draw_result = impl_->renderer.draw(impl_->transform);
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

    case image_ready_message: {
      std::unique_ptr<LoadedImage> loaded(
          reinterpret_cast<LoadedImage*>(lparam));
      const std::shared_ptr<AsyncLoadContext> context =
          impl_->async_load.context();
      if (!loaded || loaded->instance_token != impl_->instance_token ||
          !context ||
          !context->generation.is_current(loaded->generation)) {
        return 0;
      }

      if (!loaded->result.has_value()) {
        impl_->renderer.clear_image();
        InvalidateRect(impl_->window, nullptr, FALSE);
        return 0;
      }

      const core::ImageFrame& frame = loaded->result.value();
      auto upload_result = impl_->renderer.set_image(frame);
      if (!upload_result.has_value()) {
        impl_->renderer.clear_image();
        InvalidateRect(impl_->window, nullptr, FALSE);
        impl_->report_renderer_failure(upload_result.error());
        return 0;
      }

      RECT client{};
      if (GetClientRect(impl_->window, &client)) {
        const LONG width = client.right - client.left;
        const LONG height = client.bottom - client.top;
        if (width > 0 && height > 0) {
          impl_->transform.fit(
              {frame.width, frame.height},
              {static_cast<std::uint32_t>(width),
               static_cast<std::uint32_t>(height)});
        }
      }
      InvalidateRect(impl_->window, nullptr, FALSE);
      return 0;
    }

    case WM_CLOSE:
      impl_->shutdown_async();
      DestroyWindow(impl_->window);
      return 0;

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
