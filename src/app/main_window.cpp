#include "app/main_window.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <windowsx.h>

#include "viewer/app/input_mapping.h"
#include "viewer/app/window_state.h"
#include "viewer/core/async_shutdown.h"
#include "viewer/core/cancellation.h"
#include "viewer/core/directory_navigator.h"
#include "viewer/core/format_probe.h"
#include "viewer/core/image_frame.h"
#include "viewer/core/load_generation.h"
#include "viewer/core/load_metrics.h"
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
constexpr UINT navigator_ready_message = WM_APP + 2;
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
  core::LoadMetrics metrics;
  core::Result<core::ImageFrame> result;
};

struct LoadedNavigator {
  std::uint64_t instance_token;
  std::uint64_t generation;
  std::filesystem::path selected_path;
  core::Result<core::DirectoryNavigator> result;
};

struct PendingLoadDebugInfo {
  std::filesystem::path path;
  core::LoadMetrics metrics;
  bool success = false;
  std::optional<core::Error> error;
};

struct AsyncLoadContext {
  explicit AsyncLoadContext(std::uint64_t instance_token)
      : executor(worker_thread_count()),
        completions(nullptr, instance_token, false) {}

  core::LoadGeneration generation;
  core::LoadGeneration scan_generation;
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

void output_load_debug_string(const PendingLoadDebugInfo& pending) {
  const auto decode_us =
      std::chrono::duration_cast<std::chrono::microseconds>(
          pending.metrics.decode_duration())
          .count();
  const auto total_us =
      std::chrono::duration_cast<std::chrono::microseconds>(
          pending.metrics.total_duration())
          .count();

  std::wstring message = L"load path=\"" + pending.path.wstring() +
                         L"\" decode_us=" + std::to_wstring(decode_us) +
                         L" total_us=" + std::to_wstring(total_us);
  message += pending.success ? L" result=success" : L" result=failure";
  if (pending.error.has_value()) {
    message += L" error=\"";
    message += pending.error->message;
    message += L"\"";
  }
  message += L"\n";
  OutputDebugStringW(message.c_str());
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
  std::shared_ptr<core::ImageFrame> displayed_frame;
  core::SizeU displayed_size{};
  std::wstring status_text;
  bool recovering_renderer = false;
  std::optional<PendingLoadDebugInfo> pending_load_debug;
  std::optional<core::DirectoryNavigator> navigator;
  core::ThreeFrameCache frame_cache;
  core::PrefetchTracker prefetches;
  WheelDeltaAccumulator wheel_delta;
  PanTracker pan;

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

  void reset_displayed_frame() noexcept {
    displayed_frame.reset();
    displayed_size = {};
  }

  void set_displayed_frame(std::shared_ptr<core::ImageFrame> frame) {
    displayed_size = {frame->width, frame->height};
    displayed_frame = std::move(frame);
  }

  void set_renderer_status_text(std::wstring text) {
    status_text = std::move(text);
    renderer.set_status_text(status_text);
  }

  void fit_displayed_frame() {
    if (displayed_size.width == 0 || displayed_size.height == 0) {
      return;
    }

    RECT client{};
    if (GetClientRect(window, &client)) {
      const LONG width = client.right - client.left;
      const LONG height = client.bottom - client.top;
      if (width > 0 && height > 0) {
        transform.fit(displayed_size,
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
    if (!handle_upload_result(upload_result, frame)) {
      return true;
    }

    set_renderer_status_text(L"");
    pending_load_debug.reset();
    set_displayed_frame(frame);
    fit_displayed_frame();
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
    core::LoadMetrics metrics{
        .requested = core::LoadMetrics::Clock::now(),
    };
    if (display_when_ready) {
      pending_load_debug.reset();
    }

    const bool submitted = context->executor.submit(
        priority,
        [context, token, generation, purpose, display_when_ready,
         cancellation, path, metrics] mutable {
          if (cancellation.is_cancelled()) {
            return;
          }
          if (display_when_ready &&
              !context->generation.is_current(generation)) {
            return;
          }

          metrics.decode_started = core::LoadMetrics::Clock::now();
          auto decoded = [&path] {
            auto format = core::probe_file_header(path);
            if (!format.has_value()) {
              return core::Result<core::ImageFrame>::failure(
                  std::move(format).error());
            }

            const platform::WicDecoder decoder;
            return decoder.decode(path, decode_byte_budget);
          }();
          metrics.decode_finished = core::LoadMetrics::Clock::now();
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
              .metrics = metrics,
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

  void request_directory_scan(const std::filesystem::path& path) {
    const std::shared_ptr<AsyncLoadContext> context = async_load;
    if (window == nullptr || !context) {
      return;
    }

    const std::uint64_t generation = context->scan_generation.begin();
    const core::CancellationToken cancellation =
        context->cancellation.token();
    const std::uint64_t token = instance_token;

    static_cast<void>(context->executor.submit(
        core::Priority::background,
        [context, token, generation, cancellation, path] mutable {
          if (cancellation.is_cancelled() ||
              !context->scan_generation.is_current(generation)) {
            return;
          }

          auto navigator = core::DirectoryNavigator::scan(path);
          if (cancellation.is_cancelled() ||
              !context->scan_generation.is_current(generation)) {
            return;
          }

          auto payload = std::make_unique<LoadedNavigator>(
              LoadedNavigator{
                  .instance_token = token,
                  .generation = generation,
                  .selected_path = path,
                  .result = std::move(navigator),
              });
          static_cast<void>(context->completions.publish(
              payload,
              [](HWND target, std::uint64_t gate_token,
                 LoadedNavigator* loaded) {
                if (loaded->instance_token != gate_token) {
                  return false;
                }
                return PostMessageW(
                           target, navigator_ready_message, 0,
                           reinterpret_cast<LPARAM>(loaded)) != FALSE;
              }));
        }));
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

  void end_pan() noexcept {
    pan.end();
    if (GetCapture() == window) {
      ReleaseCapture();
    }
  }

  bool try_recover_renderer() {
    if (recovering_renderer || window == nullptr) {
      return false;
    }

    recovering_renderer = true;
    auto initialize_result = renderer.initialize(window);
    recovering_renderer = false;
    if (!initialize_result.has_value()) {
      renderer_ready = false;
      return false;
    }

    renderer_ready = true;
    renderer.set_status_text(status_text);

    if (displayed_frame) {
      auto upload_result = renderer.set_image(*displayed_frame);
      if (!upload_result.has_value()) {
        renderer.clear_image();
        reset_displayed_frame();
        set_renderer_status_text(L"Renderer reset failed.");
        InvalidateRect(window, nullptr, FALSE);
        return false;
      }
    }

    InvalidateRect(window, nullptr, FALSE);
    return true;
  }

  void show_renderer_reset_failed() {
    renderer.clear_image();
    reset_displayed_frame();
    set_renderer_status_text(L"Renderer reset failed.");
    InvalidateRect(window, nullptr, FALSE);
  }

  bool handle_resize_result(unsigned width,
                            unsigned height,
                            core::Result<bool>& resize_result) {
    if (resize_result.has_value()) {
      return true;
    }
    const core::Error error = resize_result.error();
    if (error.code == core::ErrorCode::render_target_lost &&
        try_recover_renderer()) {
      auto retry_result = renderer.resize(width, height);
      if (retry_result.has_value()) {
        return true;
      }
      if (retry_result.error().code ==
          core::ErrorCode::render_target_lost) {
        show_renderer_reset_failed();
        return false;
      }
      report_renderer_failure(retry_result.error());
      return false;
    }
    report_renderer_failure(error);
    return false;
  }

  bool handle_upload_result(core::Result<bool>& upload_result,
                            const std::shared_ptr<core::ImageFrame>& frame) {
    if (upload_result.has_value()) {
      return true;
    }
    const core::Error error = upload_result.error();
    if (error.code == core::ErrorCode::render_target_lost &&
        try_recover_renderer()) {
      auto retry_result = renderer.set_image(*frame);
      if (retry_result.has_value()) {
        return true;
      }
      if (retry_result.error().code ==
          core::ErrorCode::render_target_lost) {
        show_renderer_reset_failed();
        return false;
      }
      report_renderer_failure(retry_result.error());
      return false;
    }

    renderer.clear_image();
    reset_displayed_frame();
    pending_load_debug.reset();
    InvalidateRect(window, nullptr, FALSE);
    report_renderer_failure(error);
    return false;
  }

  void handle_draw_error(const core::Error& error) {
    if (error.code == core::ErrorCode::render_target_lost &&
        try_recover_renderer()) {
      InvalidateRect(window, nullptr, FALSE);
      return;
    }
    report_renderer_failure(error);
  }

  void report_renderer_failure(const core::Error& error) {
    if (error.code == core::ErrorCode::render_target_lost) {
      if (try_recover_renderer()) {
        return;
      }
      show_renderer_reset_failed();
      return;
    }

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
    while (PeekMessageW(&message, window, navigator_ready_message,
                        navigator_ready_message, PM_REMOVE)) {
      delete reinterpret_cast<LoadedNavigator*>(message.lParam);
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

  impl_->request_image(path, core::Priority::current_image, true);
  impl_->request_directory_scan(path);
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
      impl_->handle_resize_result(width, height, resize_result);
      return 0;
    }

    case WM_PAINT: {
      PAINTSTRUCT paint{};
      BeginPaint(impl_->window, &paint);

      std::optional<core::Error> draw_error;
      bool draw_succeeded = false;
      if (impl_->renderer_ready) {
        auto draw_result = impl_->renderer.draw(impl_->transform);
        if (!draw_result.has_value()) {
          draw_error = std::move(draw_result.error());
        } else {
          draw_succeeded = draw_result.value();
        }
      }

      EndPaint(impl_->window, &paint);
      if (draw_error.has_value()) {
        impl_->handle_draw_error(*draw_error);
        return 0;
      }
      if (draw_succeeded && impl_->pending_load_debug.has_value()) {
        PendingLoadDebugInfo& pending = *impl_->pending_load_debug;
        pending.metrics.presented = core::LoadMetrics::Clock::now();
        output_load_debug_string(pending);
        impl_->pending_load_debug.reset();
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
          const core::Error& error = loaded->result.error();
          impl_->renderer.clear_image();
          impl_->set_renderer_status_text(
              L"This image could not be opened.");
          impl_->reset_displayed_frame();
          impl_->pending_load_debug = PendingLoadDebugInfo{
              .path = loaded->path,
              .metrics = loaded->metrics,
              .success = false,
              .error = error,
          };
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
      if (!impl_->handle_upload_result(upload_result, frame)) {
        return 0;
      }

      impl_->set_renderer_status_text(L"");
      impl_->set_displayed_frame(frame);
      impl_->fit_displayed_frame();
      impl_->pending_load_debug = PendingLoadDebugInfo{
          .path = loaded->path,
          .metrics = loaded->metrics,
          .success = true,
      };
      InvalidateRect(impl_->window, nullptr, FALSE);
      impl_->prefetch_neighbors();
      return 0;
    }

    case navigator_ready_message: {
      std::unique_ptr<LoadedNavigator> loaded(
          reinterpret_cast<LoadedNavigator*>(lparam));
      const std::shared_ptr<AsyncLoadContext> context =
          impl_->async_load;
      if (!loaded || loaded->instance_token != impl_->instance_token ||
          !context ||
          !context->scan_generation.is_current(loaded->generation)) {
        return 0;
      }

      if (!loaded->result.has_value()) {
        return 0;
      }

      core::DirectoryNavigator navigator =
          std::move(loaded->result).value();
      if (!core::paths_match(navigator.current(), loaded->selected_path)) {
        return 0;
      }

      impl_->navigator = std::move(navigator);
      impl_->retain_relevant_frames();
      impl_->prefetch_neighbors();
      return 0;
    }

    case WM_MOUSEWHEEL: {
      const int steps = impl_->wheel_delta.consume(
          GET_WHEEL_DELTA_WPARAM(wparam));
      if (steps != 0) {
        impl_->transform.zoom_by(
            static_cast<float>(std::pow(1.1, steps)));
        InvalidateRect(impl_->window, nullptr, FALSE);
      }
      return 0;
    }

    case WM_LBUTTONDOWN:
      impl_->pan.begin(
          {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
      SetCapture(impl_->window);
      return 0;

    case WM_MOUSEMOVE:
      if (const std::optional<PanDelta> delta = impl_->pan.move_to(
              {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
          delta.has_value()) {
        if (delta->dx != 0 || delta->dy != 0) {
          impl_->transform.pan_by(static_cast<float>(delta->dx),
                                  static_cast<float>(delta->dy));
          InvalidateRect(impl_->window, nullptr, FALSE);
        }
        return 0;
      }
      break;

    case WM_LBUTTONUP:
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
      impl_->end_pan();
      return 0;

    case WM_KEYDOWN:
      switch (classify_key(static_cast<unsigned>(wparam))) {
        case KeyAction::previous:
          impl_->navigate_previous();
          return 0;

        case KeyAction::next:
          impl_->navigate_next();
          return 0;

        case KeyAction::fit:
          impl_->fit_displayed_frame();
          InvalidateRect(impl_->window, nullptr, FALSE);
          return 0;

        case KeyAction::one_to_one:
          impl_->transform.one_to_one();
          InvalidateRect(impl_->window, nullptr, FALSE);
          return 0;

        case KeyAction::rotate_clockwise:
          impl_->transform.rotate_clockwise();
          InvalidateRect(impl_->window, nullptr, FALSE);
          return 0;

        case KeyAction::none:
          break;
      }
      break;

    case WM_CLOSE:
      impl_->end_pan();
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
