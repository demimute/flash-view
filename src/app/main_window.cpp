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
#include "viewer/core/directory_navigator.h"
#include "viewer/core/image_frame.h"
#include "viewer/core/load_generation.h"
#include "viewer/core/priority_executor.h"
#include "viewer/core/result.h"
#include "viewer/core/three_frame_cache.h"
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
  core::LoadedImagePurpose purpose;
  bool display_when_ready;
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

struct ShutdownPayload {
  std::shared_ptr<AsyncLoadContext> context;
};

void CALLBACK reap_async_load_context(
    PTP_CALLBACK_INSTANCE, void* raw_payload) noexcept {
  std::unique_ptr<ShutdownPayload> payload(
      static_cast<ShutdownPayload*>(raw_payload));
  try {
    payload->context->executor.stop();
  } catch (...) {
    // Threadpool callbacks must never allow C++ exceptions to escape.
  }
}

}  // namespace

struct MainWindow::Impl {
  HWND window = nullptr;
  bool renderer_ready = false;
  const std::uint64_t instance_token = next_instance_token();
  std::shared_ptr<AsyncLoadContext> async_load;
  std::unique_ptr<core::ShutdownHandoff<ShutdownPayload>>
      shutdown_handoff;
  render::D3dRenderer renderer;
  core::ViewTransform transform;
  std::optional<core::DirectoryNavigator> navigator;
  core::ThreeFrameCache frame_cache;
  core::PrefetchTracker prefetches;

  [[nodiscard]] std::filesystem::path current_path() const {
    if (navigator.has_value()) {
      return navigator->current();
    }
    return {};
  }

  [[nodiscard]] std::filesystem::path previous_path() const {
    if (!navigator.has_value() || navigator->items().empty()) {
      return current_path();
    }
    const auto& items = navigator->items();
    const std::size_t index = navigator->current_index();
    return items[index == 0 ? items.size() - 1 : index - 1];
  }

  [[nodiscard]] std::filesystem::path next_path() const {
    if (!navigator.has_value() || navigator->items().empty()) {
      return current_path();
    }
    const auto& items = navigator->items();
    const std::size_t index = navigator->current_index();
    return items[(index + 1) % items.size()];
  }

  void fit_to_frame(const core::ImageFrame& frame) {
    RECT client{};
    if (GetClientRect(window, &client)) {
      const LONG width = client.right - client.left;
      const LONG height = client.bottom - client.top;
      if (width > 0 && height > 0) {
        transform.fit({frame.width, frame.height},
                      {static_cast<std::uint32_t>(width),
                       static_cast<std::uint32_t>(height)});
      }
    }
  }

  bool display_cached(const std::filesystem::path& path) {
    const std::shared_ptr<core::ImageFrame> frame =
        frame_cache.find(path);
    if (!frame) {
      return false;
    }

    auto upload_result = renderer.set_image(*frame);
    if (!upload_result.has_value()) {
      renderer.clear_image();
      InvalidateRect(window, nullptr, FALSE);
      report_renderer_failure(upload_result.error());
      return true;
    }

    fit_to_frame(*frame);
    InvalidateRect(window, nullptr, FALSE);
    return true;
  }

  void retain_relevant_frames() {
    const std::filesystem::path current = current_path();
    if (current.empty()) {
      return;
    }
    frame_cache.retain(current, previous_path(), next_path());
  }

  void request_image(const std::filesystem::path& path,
                     core::Priority priority,
                     bool display_when_ready) {
    const std::shared_ptr<AsyncLoadContext> context = async_load;
    if (window == nullptr || !context) {
      return;
    }

    const core::LoadedImagePurpose purpose =
        display_when_ready ? core::LoadedImagePurpose::display
                           : core::LoadedImagePurpose::prefetch;
    const std::uint64_t generation =
        display_when_ready ? context->generation.begin()
                           : context->generation.current();
    const core::CancellationToken cancellation =
        context->cancellation.token();
    const std::uint64_t token = instance_token;

    const bool submitted = context->executor.submit(
        priority,
        [context, token, generation, purpose, display_when_ready,
         cancellation, path] {
          if (cancellation.is_cancelled()) {
            return;
          }
          if (display_when_ready &&
              !context->generation.is_current(generation)) {
            return;
          }

          const platform::WicDecoder decoder;
          auto decoded = decoder.decode(path, decode_byte_budget);
          if (cancellation.is_cancelled()) {
            return;
          }
          if (display_when_ready &&
              !context->generation.is_current(generation)) {
            return;
          }

          auto payload = std::make_unique<LoadedImage>(LoadedImage{
              .instance_token = token,
              .generation = generation,
              .purpose = purpose,
              .display_when_ready = display_when_ready,
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
      if (display_when_ready) {
        MessageBoxW(window, L"Unable to schedule image decoding.",
                    window_title, MB_OK | MB_ICONERROR);
      } else {
        prefetches.finish(path);
      }
    }
  }

  void prefetch_neighbors() {
    if (!navigator.has_value()) {
      return;
    }

    const std::filesystem::path previous = previous_path();
    const std::filesystem::path next = next_path();
    if (prefetches.should_submit(previous, frame_cache)) {
      request_image(previous, core::Priority::adjacent_image, false);
    }
    if (!core::paths_match(next, previous) &&
        prefetches.should_submit(next, frame_cache)) {
      request_image(next, core::Priority::adjacent_image, false);
    }
  }

  void show_current_or_request() {
    const std::filesystem::path current = current_path();
    if (current.empty()) {
      return;
    }

    retain_relevant_frames();
    if (!display_cached(current)) {
      request_image(current, core::Priority::current_image, true);
    }
    prefetch_neighbors();
  }

  void navigate_previous() {
    if (!navigator.has_value()) {
      return;
    }
    static_cast<void>(navigator->previous());
    show_current_or_request();
  }

  void navigate_next() {
    if (!navigator.has_value()) {
      return;
    }
    static_cast<void>(navigator->next());
    show_current_or_request();
  }

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

  void shutdown_async() noexcept {
    if (!shutdown_handoff) {
      return;
    }

    ShutdownPayload* const payload = shutdown_handoff->take();
    if (payload == nullptr) {
      return;
    }

    std::shared_ptr<AsyncLoadContext> context = payload->context;
    async_load.reset();
    static_cast<void>(context->completions.close());
    context->cancellation.cancel();
    discard_pending_completions();

    if (!TrySubmitThreadpoolCallback(
            &reap_async_load_context, payload, nullptr)) {
      // Extreme resource exhaustion: retain the released, preallocated
      // payload as a bounded process-lifetime leak (at most one per window).
      // Its closed gate prevents window access, and the OS reclaims it at
      // process exit. Blocking or throwing from WM_CLOSE would be worse.
    }
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
  try {
    impl_->async_load =
        std::make_shared<AsyncLoadContext>(impl_->instance_token);
    impl_->shutdown_handoff = std::make_unique<
        core::ShutdownHandoff<ShutdownPayload>>(
        std::make_unique<ShutdownPayload>(
            ShutdownPayload{impl_->async_load}));
  } catch (...) {
    return false;
  }

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

  impl_->async_load->completions.activate(
      window, impl_->instance_token);

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
      impl_->async_load;
  if (impl_->window == nullptr || !context) {
    return;
  }

  context->cancellation.cancel();
  context->cancellation = core::CancellationSource{};
  impl_->prefetches.clear();
  impl_->navigator.reset();

  auto navigator = core::DirectoryNavigator::scan(path);
  if (navigator.has_value()) {
    impl_->navigator = std::move(navigator).value();
    impl_->show_current_or_request();
  } else {
    impl_->request_image(path, core::Priority::current_image, true);
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
          impl_->async_load;
      if (!loaded || loaded->instance_token != impl_->instance_token ||
          !context) {
        return 0;
      }
      if (loaded->purpose == core::LoadedImagePurpose::prefetch) {
        impl_->prefetches.finish(loaded->path);
      }

      const std::filesystem::path current = impl_->current_path();
      const std::filesystem::path previous = impl_->previous_path();
      const std::filesystem::path next = impl_->next_path();
      if (loaded->display_when_ready) {
        if (!context->generation.is_current(loaded->generation) ||
            (!current.empty() &&
             !core::paths_match(loaded->path, current))) {
          return 0;
        }
      } else if (current.empty() ||
                 !core::should_accept_loaded_image(
                     loaded->purpose, loaded->path, current, previous,
                     next, loaded->generation,
                     context->generation.current())) {
        return 0;
      }

      if (!loaded->result.has_value()) {
        if (loaded->display_when_ready) {
          impl_->renderer.clear_image();
          InvalidateRect(impl_->window, nullptr, FALSE);
        }
        return 0;
      }

      auto frame = std::make_shared<core::ImageFrame>(
          std::move(loaded->result).value());
      impl_->frame_cache.remember(loaded->path, frame);
      impl_->retain_relevant_frames();

      const bool should_display =
          loaded->display_when_ready ||
          core::paths_match(loaded->path, current);

      if (!should_display) {
        return 0;
      }

      auto upload_result = impl_->renderer.set_image(*frame);
      if (!upload_result.has_value()) {
        impl_->renderer.clear_image();
        InvalidateRect(impl_->window, nullptr, FALSE);
        impl_->report_renderer_failure(upload_result.error());
        return 0;
      }

      impl_->fit_to_frame(*frame);
      InvalidateRect(impl_->window, nullptr, FALSE);
      impl_->prefetch_neighbors();
      return 0;
    }

    case WM_KEYDOWN:
      switch (wparam) {
        case VK_LEFT:
        case VK_UP:
        case VK_PRIOR:
          impl_->navigate_previous();
          return 0;

        case VK_RIGHT:
        case VK_DOWN:
        case VK_NEXT:
        case VK_SPACE:
          impl_->navigate_next();
          return 0;

        default:
          break;
      }
      break;

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

  return DefWindowProcW(impl_->window, message, wparam, lparam);
}

}  // namespace viewer::app
