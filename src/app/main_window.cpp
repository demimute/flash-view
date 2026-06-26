#include "app/main_window.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <cmath>
#include <memory>
#include <optional>
#include <unordered_map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include "app/resource.h"

#include "viewer/app/input_mapping.h"
#include "viewer/app/render_failure_policy.h"
#include "viewer/app/window_state.h"
#include "viewer/core/async_shutdown.h"
#include "viewer/core/cancellation.h"
#include "viewer/core/directory_navigator.h"
#include "viewer/core/format_probe.h"
#include "viewer/core/image_frame.h"
#include "viewer/core/load_generation.h"
#include "viewer/core/load_metrics.h"
#include "viewer/core/natural_sort.h"
#include "viewer/core/priority_executor.h"
#include "viewer/core/result.h"
#include "viewer/core/three_frame_cache.h"
#include "viewer/core/thumbnail_layout.h"
#include "viewer/core/view_transform.h"
#include "viewer/platform/wic_decoder.h"
#include "viewer/render/d3d_renderer.h"

namespace viewer::app {
namespace {

constexpr wchar_t window_class_name[] = L"FastImageViewer.MainWindow";
constexpr wchar_t window_title[] = L"Fast Image Viewer";
constexpr wchar_t empty_prompt[] =
    L"Drop image here or press O to open.";
constexpr UINT image_ready_message = WM_APP + 1;
constexpr UINT navigator_ready_message = WM_APP + 2;
constexpr UINT_PTR animation_timer_id = 2000;
constexpr UINT_PTR toolbar_first_id = 3000;
constexpr UINT_PTR toolbar_hide_timer_id = 2001;
constexpr UINT toolbar_hide_delay_ms = 6000;
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
  std::optional<core::AnimatedImage> animation;
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

struct ToolbarButton {
  UINT_PTR id;
};

constexpr std::array toolbar_buttons{
    ToolbarButton{toolbar_first_id + 0},
    ToolbarButton{toolbar_first_id + 1},
    ToolbarButton{toolbar_first_id + 2},
    ToolbarButton{toolbar_first_id + 3},
    ToolbarButton{toolbar_first_id + 4},
    ToolbarButton{toolbar_first_id + 5},
    ToolbarButton{toolbar_first_id + 6},
    ToolbarButton{toolbar_first_id + 7},
    ToolbarButton{toolbar_first_id + 8},
    ToolbarButton{toolbar_first_id + 9},
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

std::wstring single_quote_powershell(std::wstring value) {
  std::wstring escaped = L"'";
  for (const wchar_t ch : value) {
    if (ch == L'\'') {
      escaped += L"''";
    } else {
      escaped += ch;
    }
  }
  escaped += L"'";
  return escaped;
}

std::wstring lower_extension(const std::filesystem::path& path) {
  std::wstring extension = path.extension().wstring();
  for (wchar_t& ch : extension) {
    ch = static_cast<wchar_t>(std::towlower(ch));
  }
  return extension;
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
  core::ThumbnailLayoutState thumbnail_layout;
  bool toolbar_visible = false;
  std::array<RECT, toolbar_buttons.size()> toolbar_button_rects{};
  bool resizing_thumbnail_panel = false;
  int thumbnail_scroll_offset = 0;
  enum class ThumbnailBrowserEntryKind {
    parent,
    folder,
    image,
  };
  struct ThumbnailBrowserEntry {
    ThumbnailBrowserEntryKind kind = ThumbnailBrowserEntryKind::image;
    std::filesystem::path path;
  };
  std::vector<ThumbnailBrowserEntry> thumbnail_entries_cache;
  std::filesystem::path thumbnail_entries_directory;
  std::optional<std::filesystem::path> thumbnail_browser_directory;
  bool thumbnail_entries_dirty = true;
  std::vector<std::filesystem::path> temporary_archive_dirs;
  std::unordered_map<std::wstring, std::shared_ptr<core::ImageFrame>>
      thumbnail_cache;
  std::shared_ptr<core::AnimatedImage> current_animation;
  std::size_t current_animation_index = 0;

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

  void stop_animation() noexcept {
    if (window != nullptr) {
      KillTimer(window, animation_timer_id);
    }
    current_animation.reset();
    current_animation_index = 0;
  }

  void set_displayed_frame(std::shared_ptr<core::ImageFrame> frame) {
    displayed_size = {frame->width, frame->height};
    displayed_frame = std::move(frame);
  }

  void set_renderer_status_text(std::wstring text) {
    status_text = std::move(text);
    renderer.set_status_text(status_text);
  }

  void show_empty_prompt() {
    renderer.clear_image();
    reset_displayed_frame();
    stop_animation();
    toolbar_visible = false;
    KillTimer(window, toolbar_hide_timer_id);
    pending_load_debug.reset();
    set_renderer_status_text(empty_prompt);
    InvalidateRect(window, nullptr, FALSE);
  }

  void create_child_controls() {
    update_child_layout();
  }

  void update_child_layout() {
    InvalidateRect(window, nullptr, FALSE);
  }

  [[nodiscard]] RECT client_rect() const noexcept {
    RECT client{};
    if (window != nullptr) {
      GetClientRect(window, &client);
    }
    return client;
  }

  [[nodiscard]] RECT thumbnail_panel_rect() const noexcept {
    RECT client = client_rect();
    if (!thumbnail_layout.visible) {
      return RECT{};
    }

    const int panel = static_cast<int>(thumbnail_layout.panel_extent);
    switch (thumbnail_layout.dock) {
      case core::ThumbnailDock::bottom:
        client.top = (std::max)(client.top, client.bottom - panel);
        return client;
      case core::ThumbnailDock::top:
        client.bottom = (std::min)(client.bottom, client.top + panel);
        return client;
      case core::ThumbnailDock::left:
        client.right = (std::min)(client.right, client.left + panel);
        return client;
      case core::ThumbnailDock::right:
        client.left = (std::max)(client.left, client.right - panel);
        return client;
    }
    return client;
  }

  [[nodiscard]] RECT image_area_rect() const noexcept {
    RECT client = client_rect();
    if (!thumbnail_layout.visible) {
      return client;
    }
    const int panel = static_cast<int>(thumbnail_layout.panel_extent);
    switch (thumbnail_layout.dock) {
      case core::ThumbnailDock::bottom:
        client.bottom = (std::max)(client.top, client.bottom - panel);
        return client;
      case core::ThumbnailDock::top:
        client.top = (std::min)(client.bottom, client.top + panel);
        return client;
      case core::ThumbnailDock::left:
        client.left = (std::min)(client.right, client.left + panel);
        return client;
      case core::ThumbnailDock::right:
        client.right = (std::max)(client.left, client.right - panel);
        return client;
    }
    return client;
  }

  [[nodiscard]] std::optional<RECT> thumbnail_splitter_rect() const {
    if (window == nullptr || !thumbnail_layout.visible) {
      return std::nullopt;
    }
    constexpr int splitter = 6;
    RECT panel = thumbnail_panel_rect();
    switch (thumbnail_layout.dock) {
      case core::ThumbnailDock::bottom:
        return RECT{panel.left, panel.top, panel.right, panel.top + splitter};
      case core::ThumbnailDock::top:
        return RECT{panel.left, panel.bottom - splitter, panel.right,
                    panel.bottom};
      case core::ThumbnailDock::left:
        return RECT{panel.right - splitter, panel.top, panel.right,
                    panel.bottom};
      case core::ThumbnailDock::right:
        return RECT{panel.left, panel.top, panel.left + splitter,
                    panel.bottom};
    }
    return std::nullopt;
  }

  [[nodiscard]] bool begin_thumbnail_panel_resize(Point point) {
    const auto splitter = thumbnail_splitter_rect();
    if (!splitter.has_value()) {
      return false;
    }
    POINT native_point{point.x, point.y};
    if (PtInRect(&*splitter, native_point) == FALSE) {
      return false;
    }
    resizing_thumbnail_panel = true;
    SetCapture(window);
    return true;
  }

  [[nodiscard]] bool is_on_thumbnail_splitter(Point point) const {
    const auto splitter = thumbnail_splitter_rect();
    if (!splitter.has_value()) {
      return false;
    }
    POINT native_point{point.x, point.y};
    return PtInRect(&*splitter, native_point) != FALSE;
  }

  [[nodiscard]] LPCWSTR thumbnail_resize_cursor() const noexcept {
    switch (thumbnail_layout.dock) {
      case core::ThumbnailDock::bottom:
      case core::ThumbnailDock::top:
        return IDC_SIZENS;
      case core::ThumbnailDock::left:
      case core::ThumbnailDock::right:
        return IDC_SIZEWE;
    }
    return IDC_ARROW;
  }

  void resize_thumbnail_panel_to(Point point) {
    if (!resizing_thumbnail_panel) {
      return;
    }
    const RECT client = client_rect();
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    std::uint32_t requested = thumbnail_layout.panel_extent;
    switch (thumbnail_layout.dock) {
      case core::ThumbnailDock::bottom:
        requested = static_cast<std::uint32_t>((std::max)(0, height - point.y));
        break;
      case core::ThumbnailDock::top:
        requested = static_cast<std::uint32_t>((std::max)(0, point.y));
        break;
      case core::ThumbnailDock::left:
        requested = static_cast<std::uint32_t>((std::max)(0, point.x));
        break;
      case core::ThumbnailDock::right:
        requested = static_cast<std::uint32_t>((std::max)(0, width - point.x));
        break;
    }
    thumbnail_layout.resize_panel(requested);
    clamp_thumbnail_scroll();
    fit_displayed_frame();
    update_child_layout();
  }

  void refresh_thumbnail_items() {
    thumbnail_entries_dirty = true;
    clamp_thumbnail_scroll();
    InvalidateRect(window, nullptr, FALSE);
  }

  [[nodiscard]] std::filesystem::path thumbnail_directory() const {
    if (thumbnail_browser_directory.has_value()) {
      return *thumbnail_browser_directory;
    }
    const std::filesystem::path current = current_path();
    if (!current.empty() && current.has_parent_path()) {
      return current.parent_path();
    }
    return {};
  }

  void rebuild_thumbnail_entries() {
    const std::filesystem::path directory = thumbnail_directory();
    const bool same_directory =
        thumbnail_entries_directory == directory ||
        (!thumbnail_entries_directory.empty() && !directory.empty() &&
         core::paths_match(thumbnail_entries_directory, directory));
    if (!thumbnail_entries_dirty && same_directory) {
      return;
    }

    thumbnail_entries_cache.clear();
    thumbnail_entries_directory = directory;
    thumbnail_entries_dirty = false;

    if (directory.empty()) {
      return;
    }

    std::error_code error;
    const auto directory_status = std::filesystem::status(directory, error);
    if (error || !std::filesystem::is_directory(directory_status)) {
      return;
    }

    const std::filesystem::path parent = directory.parent_path();
    if (!parent.empty() && parent != directory) {
      thumbnail_entries_cache.push_back(ThumbnailBrowserEntry{
          .kind = ThumbnailBrowserEntryKind::parent,
          .path = parent,
      });
    }

    std::vector<std::filesystem::path> folders;
    std::filesystem::directory_iterator iterator(directory, error);
    const std::filesystem::directory_iterator end;
    if (!error) {
      while (iterator != end) {
        const auto& entry = *iterator;
        const bool is_directory = entry.is_directory(error);
        if (error) {
          error.clear();
        } else if (is_directory) {
          folders.push_back(entry.path());
        }
        iterator.increment(error);
        if (error) {
          error.clear();
          break;
        }
      }
    }

    const core::NaturalLess natural_less;
    std::sort(folders.begin(), folders.end(),
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
    for (const auto& folder : folders) {
      thumbnail_entries_cache.push_back(ThumbnailBrowserEntry{
          .kind = ThumbnailBrowserEntryKind::folder,
          .path = folder,
      });
    }

    std::vector<std::filesystem::path> images;
    std::filesystem::directory_iterator image_iterator(directory, error);
    if (!error) {
      while (image_iterator != end) {
        const auto& entry = *image_iterator;
        const bool is_regular_file = entry.is_regular_file(error);
        if (error) {
          error.clear();
        } else if (is_regular_file &&
                   core::is_supported_image_extension(entry.path())) {
          images.push_back(entry.path());
        }
        image_iterator.increment(error);
        if (error) {
          error.clear();
          break;
        }
      }
    }

    std::sort(images.begin(), images.end(),
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
    for (const auto& image : images) {
      thumbnail_entries_cache.push_back(ThumbnailBrowserEntry{
          .kind = ThumbnailBrowserEntryKind::image,
          .path = image,
      });
    }
  }

  void set_thumbnail_browser_directory(std::filesystem::path directory) {
    thumbnail_browser_directory = std::move(directory);
    thumbnail_scroll_offset = 0;
    thumbnail_entries_dirty = true;
    refresh_thumbnail_items();
  }

  void clear_thumbnail_browser_directory() {
    thumbnail_browser_directory.reset();
    thumbnail_scroll_offset = 0;
    thumbnail_entries_dirty = true;
  }

  [[nodiscard]] bool selected_thumbnail_image(
      const std::filesystem::path& path) const {
    const std::filesystem::path current = current_path();
    return !current.empty() && core::paths_match(path, current);
  }

  [[nodiscard]] render::ThumbnailOverlayKind thumbnail_overlay_kind(
      ThumbnailBrowserEntryKind kind) const noexcept {
    switch (kind) {
      case ThumbnailBrowserEntryKind::folder:
        return render::ThumbnailOverlayKind::folder;
      case ThumbnailBrowserEntryKind::parent:
        return render::ThumbnailOverlayKind::parent;
      case ThumbnailBrowserEntryKind::image:
        return render::ThumbnailOverlayKind::image;
    }
    return render::ThumbnailOverlayKind::image;
  }

  [[nodiscard]] std::wstring thumbnail_entry_label(
      const ThumbnailBrowserEntry& entry) const {
    if (entry.kind == ThumbnailBrowserEntryKind::parent) {
      return L"..";
    }
    std::wstring label = entry.path.filename().wstring();
    if (label.empty()) {
      label = entry.path.wstring();
    }
    return label;
  }

  void open_thumbnail_browser_entry(const ThumbnailBrowserEntry& entry) {
    switch (entry.kind) {
      case ThumbnailBrowserEntryKind::folder:
      case ThumbnailBrowserEntryKind::parent:
        set_thumbnail_browser_directory(entry.path);
        return;

      case ThumbnailBrowserEntryKind::image:
        clear_thumbnail_browser_directory();
        open_path(entry.path);
        return;
      }
    }
  }

  [[nodiscard]] const std::vector<ThumbnailBrowserEntry>& thumbnail_entries() {
    rebuild_thumbnail_entries();
    return thumbnail_entries_cache;
  }

  struct ThumbnailGridMetrics {
    int thumb = 0;
    int cell_width = 0;
    int cell_height = 0;
    int padding = 14;
    int columns = 1;
    int rows = 0;
    int content_height = 0;
    int viewport_height = 0;
    int max_scroll = 0;
  };

  [[nodiscard]] ThumbnailGridMetrics thumbnail_grid_metrics(
      const RECT& panel) {
    ThumbnailGridMetrics metrics;
    metrics.thumb = static_cast<int>(thumbnail_layout.thumbnail_size);
    metrics.cell_width = metrics.thumb + 28;
    metrics.cell_height = metrics.thumb + 42;
    metrics.viewport_height =
        (std::max)(0, static_cast<int>(panel.bottom - panel.top));
    const int scrollbar_allowance = 14;
    const int panel_width = static_cast<int>(panel.right - panel.left);
    const int usable_width = (std::max)(
        1, panel_width - metrics.padding * 2 - scrollbar_allowance);
    metrics.columns = (std::max)(1, usable_width / metrics.cell_width);
    const int item_count = static_cast<int>(thumbnail_entries().size());
    metrics.rows = item_count == 0
                       ? 0
                       : (item_count + metrics.columns - 1) / metrics.columns;
    metrics.content_height =
        metrics.padding * 2 + metrics.rows * metrics.cell_height;
    metrics.max_scroll =
        (std::max)(0, metrics.content_height - metrics.viewport_height);
    return metrics;
  }

  void clamp_thumbnail_scroll() {
    if (!thumbnail_layout.visible || thumbnail_directory().empty()) {
      thumbnail_scroll_offset = 0;
      return;
    }
    const ThumbnailGridMetrics metrics =
        thumbnail_grid_metrics(thumbnail_panel_rect());
    thumbnail_scroll_offset =
        std::clamp(thumbnail_scroll_offset, 0, metrics.max_scroll);
  }

  [[nodiscard]] bool is_point_in_thumbnail_panel(Point point) const {
    if (!thumbnail_layout.visible) {
      return false;
    }
    const RECT panel = thumbnail_panel_rect();
    POINT native{point.x, point.y};
    return PtInRect(&panel, native) != FALSE;
  }

  bool scroll_thumbnail_panel(int steps) {
    if (steps == 0 || !thumbnail_layout.visible ||
        thumbnail_directory().empty()) {
      return false;
    }
    const ThumbnailGridMetrics metrics =
        thumbnail_grid_metrics(thumbnail_panel_rect());
    if (metrics.max_scroll == 0) {
      return false;
    }
    thumbnail_scroll_offset =
        std::clamp(thumbnail_scroll_offset -
                       steps * (std::max)(32, metrics.cell_height / 2),
                   0, metrics.max_scroll);
    InvalidateRect(window, nullptr, FALSE);
    return true;
  }

  [[nodiscard]] std::shared_ptr<core::ImageFrame> cached_thumbnail_frame_for(
      const std::filesystem::path& path) const {
    const std::wstring key = path.wstring();
    if (const auto cached = thumbnail_cache.find(key);
        cached != thumbnail_cache.end()) {
      return cached->second;
    }
    if (const auto cached_frame = frame_cache.find(path)) {
      return cached_frame;
    }
    return {};
  }

  [[nodiscard]] std::shared_ptr<core::ImageFrame> thumbnail_frame_for(
      const std::filesystem::path& path) {
    const std::wstring key = path.wstring();
    if (const auto cached = thumbnail_cache.find(key);
        cached != thumbnail_cache.end()) {
      return cached->second;
    }

    if (const auto cached_frame = frame_cache.find(path)) {
      thumbnail_cache.emplace(key, cached_frame);
      return cached_frame;
    }

    const platform::WicDecoder decoder;
    auto decoded =
        decoder.decode(path, std::size_t{256} * 1024U * 1024U);
    if (!decoded.has_value()) {
      return {};
    }

    auto frame = std::make_shared<core::ImageFrame>(
        std::move(decoded).value());
    thumbnail_cache.emplace(key, frame);
    return frame;
  }

  void draw_thumbnail_item(const DRAWITEMSTRUCT& item) {
    const bool selected =
        (item.itemState & ODS_SELECTED) == ODS_SELECTED;
    HBRUSH background = CreateSolidBrush(selected ? RGB(54, 94, 140)
                                                  : RGB(27, 30, 34));
    FillRect(item.hDC, &item.rcItem, background);
    DeleteObject(background);

    RECT thumb = item.rcItem;
    thumb.left += 8;
    thumb.top += 6;
    thumb.right = thumb.left +
                  static_cast<LONG>(thumbnail_layout.thumbnail_size);
    thumb.bottom = thumb.top +
                   static_cast<LONG>(thumbnail_layout.thumbnail_size);

    const std::filesystem::path path =
        navigator.has_value() && item.itemID < navigator->items().size()
            ? navigator->items()[item.itemID]
            : std::filesystem::path{};
    const auto frame = path.empty() ? nullptr : thumbnail_frame_for(path);
    if (frame && !frame->pixels.empty()) {
      BITMAPINFO bitmap_info{};
      bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bitmap_info.bmiHeader.biWidth = static_cast<LONG>(frame->width);
      bitmap_info.bmiHeader.biHeight = -static_cast<LONG>(frame->height);
      bitmap_info.bmiHeader.biPlanes = 1;
      bitmap_info.bmiHeader.biBitCount = 32;
      bitmap_info.bmiHeader.biCompression = BI_RGB;
      StretchDIBits(item.hDC, thumb.left, thumb.top,
                    thumb.right - thumb.left, thumb.bottom - thumb.top,
                    0, 0, static_cast<int>(frame->width),
                    static_cast<int>(frame->height), frame->pixels.data(),
                    &bitmap_info, DIB_RGB_COLORS, SRCCOPY);
    } else {
      HBRUSH thumb_brush = CreateSolidBrush(RGB(48, 52, 58));
      FillRect(item.hDC, &thumb, thumb_brush);
      DeleteObject(thumb_brush);
    }
    FrameRect(item.hDC, &thumb,
              reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));

    wchar_t text[512]{};
    SendMessageW(item.hwndItem, LB_GETTEXT, item.itemID,
                 reinterpret_cast<LPARAM>(text));
    RECT text_rect = item.rcItem;
    text_rect.left = thumb.right + 10;
    text_rect.top += 8;
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, selected ? RGB(255, 255, 255)
                                    : RGB(220, 226, 236));
    DrawTextW(item.hDC, text, -1, &text_rect,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
  }

  void update_preview_label() {
  }

  void draw_preview_item(const DRAWITEMSTRUCT& item) {
    HBRUSH background = CreateSolidBrush(RGB(21, 23, 26));
    FillRect(item.hDC, &item.rcItem, background);
    DeleteObject(background);

    const std::filesystem::path current = current_path();
    const auto frame = current.empty() ? nullptr : thumbnail_frame_for(current);
    if (!frame || frame->pixels.empty()) {
      SetBkMode(item.hDC, TRANSPARENT);
      SetTextColor(item.hDC, RGB(220, 226, 236));
      RECT text_rect = item.rcItem;
      DrawTextW(item.hDC, L"Preview", -1, &text_rect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      return;
    }

    const int box_width = item.rcItem.right - item.rcItem.left;
    const int box_height = item.rcItem.bottom - item.rcItem.top;
    const double scale = (std::min)(
        static_cast<double>(box_width) / static_cast<double>(frame->width),
        static_cast<double>(box_height) / static_cast<double>(frame->height));
    const int draw_width =
        (std::max)(1, static_cast<int>(static_cast<double>(frame->width) *
                                       scale));
    const int draw_height =
        (std::max)(1, static_cast<int>(static_cast<double>(frame->height) *
                                       scale));
    const int draw_x = item.rcItem.left + (box_width - draw_width) / 2;
    const int draw_y = item.rcItem.top + (box_height - draw_height) / 2;

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = static_cast<LONG>(frame->width);
    bitmap_info.bmiHeader.biHeight = -static_cast<LONG>(frame->height);
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(item.hDC, draw_x, draw_y, draw_width, draw_height, 0, 0,
                  static_cast<int>(frame->width),
                  static_cast<int>(frame->height), frame->pixels.data(),
                  &bitmap_info, DIB_RGB_COLORS, SRCCOPY);
  }

  void alpha_fill_rect(HDC target, RECT rect, COLORREF color,
                       BYTE alpha) const {
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
      return;
    }

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(target, &bitmap_info, DIB_RGB_COLORS,
                                      &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
      return;
    }

    const std::uint32_t pixel =
        (static_cast<std::uint32_t>(alpha) << 24U) |
        (static_cast<std::uint32_t>(GetRValue(color)) << 16U) |
        (static_cast<std::uint32_t>(GetGValue(color)) << 8U) |
        static_cast<std::uint32_t>(GetBValue(color));
    std::fill_n(static_cast<std::uint32_t*>(bits),
                static_cast<std::size_t>(width) *
                    static_cast<std::size_t>(height),
                pixel);

    HDC memory = CreateCompatibleDC(target);
    HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
    BLENDFUNCTION blend{
        .BlendOp = AC_SRC_OVER,
        .BlendFlags = 0,
        .SourceConstantAlpha = 255,
        .AlphaFormat = AC_SRC_ALPHA,
    };
    AlphaBlend(target, rect.left, rect.top, width, height, memory, 0, 0,
               width, height, blend);
    SelectObject(memory, old_bitmap);
    DeleteDC(memory);
    DeleteObject(bitmap);
  }

  [[nodiscard]] RECT toolbar_rect() const noexcept {
    RECT image = image_area_rect();
    constexpr int button = 80;
    constexpr int gap = 16;
    constexpr int padding = 20;
    constexpr int height = 104;
    const int width = padding * 2 +
                      static_cast<int>(toolbar_buttons.size()) * button +
                      static_cast<int>(toolbar_buttons.size() - 1) * gap;
    const int image_width = image.right - image.left;
    int left = image.left + (image_width - width) / 2;
    left = (std::max)(static_cast<int>(image.left) + 12, left);
    int top = image.bottom - height - 44;
    top = (std::max)(static_cast<int>(image.top) + 12, top);
    return RECT{left, top, left + width, top + height};
  }

  void layout_toolbar_buttons() noexcept {
    const RECT bar = toolbar_rect();
    constexpr int button = 80;
    constexpr int gap = 16;
    int left = bar.left + 20;
    for (std::size_t index = 0; index < toolbar_button_rects.size(); ++index) {
      toolbar_button_rects[index] =
          RECT{left, bar.top + 12, left + button, bar.top + 92};
      left += button + gap;
    }
  }

  void draw_toolbar_icon(HDC hdc, RECT rect, std::size_t index) {
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(238, 244, 252));
    HGDIOBJ old_pen = SelectObject(hdc, pen);
    HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    const int cx = (rect.left + rect.right) / 2;
    const int cy = (rect.top + rect.bottom) / 2;

    switch (index) {
      case 0:  // open folder
        MoveToEx(hdc, rect.left + 9, rect.top + 16, nullptr);
        LineTo(hdc, rect.left + 15, rect.top + 11);
        LineTo(hdc, rect.left + 23, rect.top + 11);
        LineTo(hdc, rect.left + 27, rect.top + 15);
        LineTo(hdc, rect.right - 8, rect.top + 15);
        Rectangle(hdc, rect.left + 8, rect.top + 16, rect.right - 8,
                  rect.bottom - 9);
        break;
      case 1: {
        POINT triangle[]{{cx - 9, cy}, {cx + 7, cy - 10}, {cx + 7, cy + 10}};
        Polygon(hdc, triangle, 3);
        break;
      }
      case 2: {
        POINT triangle[]{{cx + 9, cy}, {cx - 7, cy - 10}, {cx - 7, cy + 10}};
        Polygon(hdc, triangle, 3);
        break;
      }
      case 3:  // fit
        MoveToEx(hdc, rect.left + 10, rect.top + 17, nullptr);
        LineTo(hdc, rect.left + 10, rect.top + 10);
        LineTo(hdc, rect.left + 17, rect.top + 10);
        MoveToEx(hdc, rect.right - 17, rect.top + 10, nullptr);
        LineTo(hdc, rect.right - 10, rect.top + 10);
        LineTo(hdc, rect.right - 10, rect.top + 17);
        MoveToEx(hdc, rect.left + 10, rect.bottom - 17, nullptr);
        LineTo(hdc, rect.left + 10, rect.bottom - 10);
        LineTo(hdc, rect.left + 17, rect.bottom - 10);
        MoveToEx(hdc, rect.right - 17, rect.bottom - 10, nullptr);
        LineTo(hdc, rect.right - 10, rect.bottom - 10);
        LineTo(hdc, rect.right - 10, rect.bottom - 17);
        break;
      case 4:
        Rectangle(hdc, rect.left + 11, rect.top + 10, rect.right - 11,
                  rect.bottom - 10);
        TextOutW(hdc, cx - 3, cy - 8, L"1", 1);
        break;
      case 5:
        Arc(hdc, rect.left + 9, rect.top + 9, rect.right - 9,
            rect.bottom - 9, rect.right - 12, cy, cx, rect.top + 8);
        MoveToEx(hdc, rect.right - 13, cy - 7, nullptr);
        LineTo(hdc, rect.right - 8, cy);
        LineTo(hdc, rect.right - 16, cy + 2);
        break;
      case 6:
        for (int row = 0; row < 3; ++row) {
          for (int col = 0; col < 3; ++col) {
            Rectangle(hdc, rect.left + 10 + col * 7, rect.top + 10 + row * 7,
                      rect.left + 15 + col * 7, rect.top + 15 + row * 7);
          }
        }
        break;
      case 7:
        Rectangle(hdc, rect.left + 9, rect.top + 9, rect.right - 9,
                  rect.bottom - 9);
        MoveToEx(hdc, rect.left + 9, rect.bottom - 17, nullptr);
        LineTo(hdc, rect.right - 9, rect.bottom - 17);
        break;
      case 8:
        MoveToEx(hdc, cx - 8, cy, nullptr);
        LineTo(hdc, cx + 8, cy);
        MoveToEx(hdc, cx, cy - 8, nullptr);
        LineTo(hdc, cx, cy + 8);
        break;
      case 9:
        MoveToEx(hdc, cx - 8, cy, nullptr);
        LineTo(hdc, cx + 8, cy);
        break;
      default:
        break;
    }

    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(pen);
  }

  void draw_toolbar(HDC hdc) {
    if (!toolbar_visible) {
      return;
    }
    layout_toolbar_buttons();
    const RECT bar = toolbar_rect();
    alpha_fill_rect(hdc, bar, RGB(18, 21, 25), 190);
    HPEN border = CreatePen(PS_SOLID, 1, RGB(88, 96, 110));
    HGDIOBJ old_pen = SelectObject(hdc, border);
    HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, bar.left, bar.top, bar.right, bar.bottom, 14, 14);
    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(border);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(238, 244, 252));
    for (std::size_t index = 0; index < toolbar_button_rects.size(); ++index) {
      draw_toolbar_icon(hdc, toolbar_button_rects[index], index);
    }
  }

  void draw_thumbnail_grid(HDC hdc) {
    if (!thumbnail_layout.visible || !navigator.has_value()) {
      return;
    }
    const RECT panel = thumbnail_panel_rect();
    alpha_fill_rect(hdc, panel, RGB(20, 23, 28), 235);
    if (const auto splitter = thumbnail_splitter_rect()) {
      HBRUSH brush = CreateSolidBrush(RGB(80, 88, 102));
      FillRect(hdc, &*splitter, brush);
      DeleteObject(brush);
    }

    const int thumb = static_cast<int>(thumbnail_layout.thumbnail_size);
    const int cell_width = thumb + 28;
    const int cell_height = thumb + 42;
    const int padding = 14;
    const int usable_width = (panel.right - panel.left) - padding * 2;
    const int columns = (std::max)(1, usable_width / cell_width);
    SetStretchBltMode(hdc, HALFTONE);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(218, 224, 234));

    const auto& items = navigator->items();
    for (std::size_t index = 0; index < items.size(); ++index) {
      const int row = static_cast<int>(index) / columns;
      const int col = static_cast<int>(index) % columns;
      RECT cell{
          panel.left + padding + col * cell_width,
          panel.top + padding + row * cell_height,
          panel.left + padding + col * cell_width + cell_width - 10,
          panel.top + padding + row * cell_height + cell_height - 8,
      };
      if (cell.top >= panel.bottom) {
        break;
      }
      if (cell.bottom <= panel.top) {
        continue;
      }
      RECT image_rect{cell.left + 8, cell.top + 6, cell.left + 8 + thumb,
                      cell.top + 6 + thumb};
      const bool selected = index == navigator->current_index();
      HBRUSH selected_brush = CreateSolidBrush(
          selected ? RGB(52, 86, 128) : RGB(34, 38, 44));
      FillRect(hdc, &cell, selected_brush);
      DeleteObject(selected_brush);

      const auto frame = thumbnail_frame_for(items[index]);
      if (frame && !frame->pixels.empty()) {
        BITMAPINFO bitmap_info{};
        bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap_info.bmiHeader.biWidth = static_cast<LONG>(frame->width);
        bitmap_info.bmiHeader.biHeight = -static_cast<LONG>(frame->height);
        bitmap_info.bmiHeader.biPlanes = 1;
        bitmap_info.bmiHeader.biBitCount = 32;
        bitmap_info.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(hdc, image_rect.left, image_rect.top,
                      image_rect.right - image_rect.left,
                      image_rect.bottom - image_rect.top, 0, 0,
                      static_cast<int>(frame->width),
                      static_cast<int>(frame->height), frame->pixels.data(),
                      &bitmap_info, DIB_RGB_COLORS, SRCCOPY);
      }
      HPEN border = CreatePen(PS_SOLID, selected ? 2 : 1,
                              selected ? RGB(126, 170, 230)
                                       : RGB(78, 84, 94));
      HGDIOBJ old_pen = SelectObject(hdc, border);
      HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
      Rectangle(hdc, image_rect.left, image_rect.top, image_rect.right,
                image_rect.bottom);
      SelectObject(hdc, old_brush);
      SelectObject(hdc, old_pen);
      DeleteObject(border);

      RECT text_rect{cell.left + 4, image_rect.bottom + 6, cell.right - 4,
                     cell.bottom - 2};
      const std::wstring filename = items[index].filename().wstring();
      DrawTextW(hdc, filename.c_str(), -1, &text_rect,
                DT_CENTER | DT_END_ELLIPSIS | DT_SINGLELINE);
    }
  }

  void draw_overlays(HDC hdc) {
    draw_thumbnail_grid(hdc);
    draw_toolbar(hdc);
  }

  [[nodiscard]] render::RenderOverlay make_render_overlay() {
    render::RenderOverlay overlay;
    overlay.image_viewport_visible = true;
    overlay.image_viewport = image_area_rect();

    if (thumbnail_layout.visible && !thumbnail_directory().empty()) {
      clamp_thumbnail_scroll();
      const RECT panel = thumbnail_panel_rect();
      overlay.thumbnails_visible = true;
      overlay.thumbnail_panel = panel;
      if (const auto splitter = thumbnail_splitter_rect()) {
        overlay.thumbnail_splitter = *splitter;
      }

      const ThumbnailGridMetrics metrics = thumbnail_grid_metrics(panel);
      if (metrics.max_scroll > 0) {
        overlay.thumbnail_scrollbar_visible = true;
        const int track_width = 8;
        const int track_margin = 4;
        overlay.thumbnail_scrollbar_track = RECT{
            panel.right - track_width - track_margin,
            panel.top + track_margin,
            panel.right - track_margin,
            panel.bottom - track_margin,
        };
        const int track_height = (std::max)(
            1, static_cast<int>(overlay.thumbnail_scrollbar_track.bottom -
                                overlay.thumbnail_scrollbar_track.top));
        const int thumb_height = (std::max)(
            24, track_height * metrics.viewport_height /
                    (std::max)(metrics.viewport_height,
                               metrics.content_height));
        const int thumb_travel = (std::max)(0, track_height - thumb_height);
        const int thumb_top =
            overlay.thumbnail_scrollbar_track.top +
            (metrics.max_scroll == 0
                 ? 0
                 : thumbnail_scroll_offset * thumb_travel /
                       metrics.max_scroll);
        overlay.thumbnail_scrollbar_thumb = RECT{
            overlay.thumbnail_scrollbar_track.left,
            thumb_top,
            overlay.thumbnail_scrollbar_track.right,
            thumb_top + thumb_height,
        };
      }

      const auto& entries = thumbnail_entries();
      overlay.thumbnails.reserve((std::min)(entries.size(), std::size_t{128}));

      for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        const int row = static_cast<int>(index) / metrics.columns;
        const int col = static_cast<int>(index) % metrics.columns;
        RECT cell{
            panel.left + metrics.padding + col * metrics.cell_width,
            panel.top + metrics.padding + row * metrics.cell_height -
                thumbnail_scroll_offset,
            panel.left + metrics.padding + col * metrics.cell_width +
                metrics.cell_width - 10,
            panel.top + metrics.padding + row * metrics.cell_height +
                metrics.cell_height - 8 - thumbnail_scroll_offset,
        };
        if (cell.top >= panel.bottom) {
          break;
        }
        if (cell.bottom <= panel.top) {
          continue;
        }

        RECT image_rect{cell.left + 8, cell.top + 6,
                        cell.left + 8 + metrics.thumb,
                        cell.top + 6 + metrics.thumb};
        const bool is_image =
            entry.kind == ThumbnailBrowserEntryKind::image;
        const auto frame =
            !is_image
                ? nullptr
                : (resizing_thumbnail_panel
                       ? cached_thumbnail_frame_for(entry.path)
                       : thumbnail_frame_for(entry.path));
        if (frame && frame->width > 0 && frame->height > 0) {
          const double source_aspect =
              static_cast<double>(frame->width) /
              static_cast<double>(frame->height);
          int draw_width = metrics.thumb;
          int draw_height = metrics.thumb;
          if (source_aspect >= 1.0) {
            draw_height = (std::max)(
                1, static_cast<int>(
                       static_cast<double>(metrics.thumb) / source_aspect));
          } else {
            draw_width = (std::max)(
                1, static_cast<int>(
                       static_cast<double>(metrics.thumb) * source_aspect));
          }
          const int left =
              image_rect.left + (metrics.thumb - draw_width) / 2;
          const int top =
              image_rect.top + (metrics.thumb - draw_height) / 2;
          image_rect = RECT{left, top, left + draw_width, top + draw_height};
        }

        const render::ThumbnailOverlayKind overlay_kind =
            thumbnail_overlay_kind(entry.kind);
        std::wstring label = thumbnail_entry_label(entry);

        overlay.thumbnails.push_back(render::ThumbnailOverlayItem{
            .cell = cell,
            .image_bounds = image_rect,
            .frame = frame.get(),
            .label = std::move(label),
            .kind = overlay_kind,
            .selected = is_image && selected_thumbnail_image(entry.path),
        });
      }
    }

    if (toolbar_visible) {
      layout_toolbar_buttons();
      overlay.toolbar_visible = true;
      overlay.toolbar_bounds = toolbar_rect();
      overlay.toolbar_items.reserve(toolbar_button_rects.size());
      for (std::size_t index = 0; index < toolbar_button_rects.size();
           ++index) {
        overlay.toolbar_items.push_back(render::ToolbarOverlayItem{
            .bounds = toolbar_button_rects[index],
            .icon = index,
        });
      }
    }

    return overlay;
  }

  [[nodiscard]] std::optional<std::size_t> toolbar_hit(Point point) {
    if (!toolbar_visible) {
      return std::nullopt;
    }
    layout_toolbar_buttons();
    POINT native{point.x, point.y};
    for (std::size_t index = 0; index < toolbar_button_rects.size(); ++index) {
      if (PtInRect(&toolbar_button_rects[index], native) != FALSE) {
        return index;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::size_t> thumbnail_hit(Point point) {
    if (!thumbnail_layout.visible || thumbnail_directory().empty()) {
      return std::nullopt;
    }
    const RECT panel = thumbnail_panel_rect();
    POINT native{point.x, point.y};
    if (PtInRect(&panel, native) == FALSE) {
      return std::nullopt;
    }
    const ThumbnailGridMetrics metrics = thumbnail_grid_metrics(panel);
    const int local_x = point.x - panel.left - metrics.padding;
    const int local_y =
        point.y - panel.top - metrics.padding + thumbnail_scroll_offset;
    if (local_x < 0 || local_y < 0) {
      return std::nullopt;
    }
    const int col = local_x / metrics.cell_width;
    const int row = local_y / metrics.cell_height;
    if (col < 0 || col >= metrics.columns) {
      return std::nullopt;
    }
    const std::size_t index =
        static_cast<std::size_t>(row * metrics.columns + col);
    if (index >= thumbnail_entries().size()) {
      return std::nullopt;
    }
    return index;
  }

  void open_with_dialog() {
    std::array<wchar_t, 32768> path{};
    OPENFILENAMEW dialog{
        .lStructSize = sizeof(OPENFILENAMEW),
        .hwndOwner = window,
        .lpstrFilter =
            L"Images and archives\0*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff;*.ico;*.webp;*.heic;*.heif;*.avif;*.jxl;*.zip;*.cbz;*.rar;*.cbr;*.7z;*.cb7\0"
            L"Images\0*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff;*.ico;*.webp;*.heic;*.heif;*.avif;*.jxl\0"
            L"Archives\0*.zip;*.cbz;*.rar;*.cbr;*.7z;*.cb7\0"
            L"All files (*.*)\0*.*\0",
        .lpstrFile = path.data(),
        .nMaxFile = static_cast<DWORD>(path.size()),
        .lpstrTitle = L"Open image",
        .Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                 OFN_NOCHANGEDIR | OFN_HIDEREADONLY,
    };

    if (GetOpenFileNameW(&dialog) != FALSE) {
      open_path(path.data());
    }
  }

  void open_first_dropped_file(HDROP drop) {
    const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
    if (length == 0) {
      return;
    }

    std::wstring path(length + 1, L'\0');
    const UINT copied =
        DragQueryFileW(drop, 0, path.data(), static_cast<UINT>(path.size()));
    if (copied == 0) {
      return;
    }

    path.resize(copied);
    open_path(path);
  }

  void fit_displayed_frame() {
    if (displayed_size.width == 0 || displayed_size.height == 0) {
      return;
    }

    RECT client = image_area_rect();
    const LONG width = client.right - client.left;
    const LONG height = client.bottom - client.top;
    if (width > 0 && height > 0) {
      transform.fit(displayed_size,
                    {static_cast<std::uint32_t>(width),
                     static_cast<std::uint32_t>(height)});
    }
  }

  bool display_cached(const std::filesystem::path& path) {
    if (lower_extension(path) == L".gif") {
      return false;
    }
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
    stop_animation();
    set_displayed_frame(frame);
    fit_displayed_frame();
    InvalidateRect(window, nullptr, FALSE);
    return true;
  }

  void start_animation(std::optional<core::AnimatedImage>&& animation) {
    stop_animation();
    if (!animation.has_value() || !animation->animated()) {
      return;
    }
    current_animation = std::make_shared<core::AnimatedImage>(
        std::move(animation).value());
    current_animation_index = 0;
    SetTimer(window, animation_timer_id,
             current_animation->delays_ms[current_animation_index],
             nullptr);
  }

  void advance_animation() {
    if (!current_animation || !current_animation->animated()) {
      stop_animation();
      return;
    }
    current_animation_index =
        (current_animation_index + 1) % current_animation->frames.size();
    auto frame = std::make_shared<core::ImageFrame>(
        current_animation->frames[current_animation_index]);
    auto upload_result = renderer.set_image(*frame);
    if (!handle_upload_result(upload_result, frame)) {
      stop_animation();
      return;
    }
    set_displayed_frame(frame);
    InvalidateRect(window, nullptr, FALSE);
    SetTimer(window, animation_timer_id,
             current_animation->delays_ms[current_animation_index],
             nullptr);
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
         cancellation, path, metrics]() mutable {
          if (cancellation.is_cancelled()) {
            return;
          }
          if (display_when_ready &&
              !context->generation.is_current(generation)) {
            return;
          }

          metrics.decode_started = core::LoadMetrics::Clock::now();
          std::optional<core::AnimatedImage> animation;
          auto decoded = [&path, &animation]() {
            auto format = core::probe_file_header(path);
            if (!format.has_value()) {
              return core::Result<core::ImageFrame>::failure(
                  std::move(format).error());
            }

            const platform::WicDecoder decoder;
            if (format.value() == core::ImageFormat::gif) {
              auto animated = decoder.decode_animation(
                  path, decode_byte_budget);
              if (animated.has_value() && !animated.value().frames.empty()) {
                core::ImageFrame first_frame =
                    animated.value().frames.front();
                if (animated.value().animated()) {
                  animation = std::move(animated).value();
                }
                return core::Result<core::ImageFrame>::success(
                    std::move(first_frame));
              }
              if (!animated.has_value()) {
                return core::Result<core::ImageFrame>::failure(
                    std::move(animated).error());
              }
            }
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
              .animation = std::move(animation),
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

  void open_path(const std::filesystem::path& path) {
    const std::shared_ptr<AsyncLoadContext> context = async_load;
    if (window == nullptr || !context) {
      return;
    }

    if (core::is_supported_archive_extension(path)) {
      open_archive(path);
      return;
    }

    context->cancellation.cancel();
    context->cancellation = core::CancellationSource{};
    stop_animation();
    toolbar_visible = false;
    KillTimer(window, toolbar_hide_timer_id);
    prefetches.clear();
    navigator.reset();
    clear_thumbnail_browser_directory();
    thumbnail_entries_cache.clear();

    request_image(path, core::Priority::current_image, true);
    request_directory_scan(path);
  }

  void open_archive(const std::filesystem::path& path) {
    const std::wstring extension = lower_extension(path);
    if (extension != L".zip" && extension != L".cbz") {
      MessageBoxW(window,
                  L"This archive type is recognized but needs the 7-Zip/"
                  L"UnRAR engine in a later build. ZIP and CBZ work now.",
                  window_title, MB_OK | MB_ICONINFORMATION);
      return;
    }

    const auto extracted = extract_zip_archive(path);
    if (!extracted.has_value()) {
      MessageBoxW(window, extracted.error().message.c_str(), window_title,
                  MB_OK | MB_ICONERROR);
      return;
    }

    const auto first = find_first_image_recursive(extracted.value());
    if (!first.has_value()) {
      MessageBoxW(window,
                  L"The archive opened, but no supported image was found.",
                  window_title, MB_OK | MB_ICONINFORMATION);
      return;
    }

    open_path(first.value());
  }

  core::Result<std::filesystem::path> extract_zip_archive(
      const std::filesystem::path& archive) {
    wchar_t temp_buffer[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temp_buffer) == 0) {
      return core::Result<std::filesystem::path>::failure(
          {core::ErrorCode::platform_error,
           L"Could not find the temporary folder."});
    }

    const std::filesystem::path destination =
        std::filesystem::path(temp_buffer) /
        (std::wstring(L"FlashView-") + std::to_wstring(GetTickCount64()));
    std::error_code error;
    std::filesystem::create_directories(destination, error);
    if (error) {
      return core::Result<std::filesystem::path>::failure(
          {core::ErrorCode::io_error,
           L"Could not create the archive extraction folder."});
    }

    std::filesystem::path zip_path = archive;
    if (lower_extension(archive) == L".cbz") {
      zip_path = destination / L"archive.zip";
      std::filesystem::copy_file(archive, zip_path,
                                 std::filesystem::copy_options::overwrite_existing,
                                 error);
      if (error) {
        return core::Result<std::filesystem::path>::failure(
            {core::ErrorCode::io_error,
             L"Could not prepare the CBZ archive for extraction."});
      }
    }

    std::wstring command =
        L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
        L"\"Expand-Archive -LiteralPath " +
        single_quote_powershell(zip_path.wstring()) + L" -DestinationPath " +
        single_quote_powershell(destination.wstring()) + L" -Force\"";

    STARTUPINFOW startup{
        .cb = sizeof(STARTUPINFOW),
        .dwFlags = STARTF_USESHOWWINDOW,
        .wShowWindow = SW_HIDE,
    };
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> command_line(command.begin(), command.end());
    command_line.push_back(L'\0');
    if (CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                       &process) == FALSE) {
      return core::Result<std::filesystem::path>::failure(
          {core::ErrorCode::platform_error,
           L"Could not start the ZIP extraction helper."});
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    if (exit_code != 0) {
      return core::Result<std::filesystem::path>::failure(
          {core::ErrorCode::decode_error,
           L"The ZIP/CBZ archive could not be extracted."});
    }

    temporary_archive_dirs.push_back(destination);
    return core::Result<std::filesystem::path>::success(destination);
  }

  core::Result<std::filesystem::path> find_first_image_recursive(
      const std::filesystem::path& root) {
    std::vector<std::filesystem::path> images;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(root, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) {
      return core::Result<std::filesystem::path>::failure(
          {core::ErrorCode::io_error,
           L"Could not read the extracted archive folder."});
    }

    while (iterator != end) {
      if (iterator->is_regular_file(error) &&
          core::is_supported_image_extension(iterator->path())) {
        images.push_back(iterator->path());
      }
      error.clear();
      iterator.increment(error);
      if (error) {
        return core::Result<std::filesystem::path>::failure(
            {core::ErrorCode::io_error,
             L"Could not continue reading the extracted archive."});
      }
    }

    if (images.empty()) {
      return core::Result<std::filesystem::path>::failure(
          {core::ErrorCode::unsupported_format,
           L"No supported image was found in the archive."});
    }

    const core::NaturalLess natural_less;
    std::sort(images.begin(), images.end(),
              [&natural_less](const auto& lhs, const auto& rhs) {
                return natural_less(lhs.wstring(), rhs.wstring());
              });
    return core::Result<std::filesystem::path>::success(images.front());
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
        [context, token, generation, cancellation, path]() mutable {
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
    clear_thumbnail_browser_directory();
    show_current_or_request();
    refresh_thumbnail_items();
  }

  void navigate_next() {
    if (!navigator.has_value()) {
      return;
    }
    static_cast<void>(navigator->next());
    clear_thumbnail_browser_directory();
    show_current_or_request();
    refresh_thumbnail_items();
  }

  void select_thumbnail(std::size_t index) {
    const auto& entries = thumbnail_entries();
    if (index >= entries.size()) {
      return;
    }

    open_thumbnail_browser_entry(entries[index]);
    update_preview_label();
  }

  void toggle_thumbnails() {
    thumbnail_layout.toggle_visible();
    clamp_thumbnail_scroll();
    fit_displayed_frame();
    update_child_layout();
    refresh_thumbnail_items();
  }

  void cycle_thumbnail_dock() {
    thumbnail_layout.cycle_dock();
    clamp_thumbnail_scroll();
    fit_displayed_frame();
    update_child_layout();
  }

  void toggle_thumbnail_preview() {
    thumbnail_layout.preview_visible = false;
    InvalidateRect(window, nullptr, FALSE);
  }

  void grow_thumbnails() {
    thumbnail_layout.grow_thumbnails();
    clamp_thumbnail_scroll();
    update_child_layout();
  }

  void shrink_thumbnails() {
    thumbnail_layout.shrink_thumbnails();
    clamp_thumbnail_scroll();
    update_child_layout();
  }

  void invoke_toolbar(UINT_PTR id) {
    switch (id - toolbar_first_id) {
      case 0:
        open_with_dialog();
        return;
      case 1:
        navigate_previous();
        return;
      case 2:
        navigate_next();
        return;
      case 3:
        fit_displayed_frame();
        InvalidateRect(window, nullptr, FALSE);
        return;
      case 4:
        transform.one_to_one();
        InvalidateRect(window, nullptr, FALSE);
        return;
      case 5:
        transform.rotate_clockwise();
        InvalidateRect(window, nullptr, FALSE);
        return;
      case 6:
        toggle_thumbnails();
        return;
      case 7:
        cycle_thumbnail_dock();
        return;
      case 8:
        grow_thumbnails();
        return;
      case 9:
        shrink_thumbnails();
        return;
      default:
        return;
    }
  }

  void end_pan() noexcept {
    resizing_thumbnail_panel = false;
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
    switch (classify_renderer_failure(error)) {
      case RendererFailureAction::none:
        return;

      case RendererFailureAction::attempt_recovery:
        if (try_recover_renderer()) {
          return;
        }
        show_renderer_reset_failed();
        return;

      case RendererFailureAction::fatal:
        renderer_ready = false;
        MessageBoxW(window, error.message.c_str(), window_title,
                    MB_OK | MB_ICONERROR);
        PostQuitMessage(1);
        return;
    }
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

  void cleanup_temporary_archives() noexcept {
    for (const auto& directory : temporary_archive_dirs) {
      std::error_code error;
      std::filesystem::remove_all(directory, error);
    }
    temporary_archive_dirs.clear();
  }

  void cleanup_gdi_resources() noexcept {
  }
};

MainWindow::MainWindow() : impl_(std::make_unique<Impl>()) {}

MainWindow::~MainWindow() {
  impl_->shutdown_async();
  impl_->stop_animation();
  impl_->cleanup_temporary_archives();
  impl_->cleanup_gdi_resources();
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
      .hIcon = LoadIconW(instance, MAKEINTRESOURCEW(ID_APP_ICON)),
      .hCursor = LoadCursorW(nullptr, IDC_ARROW),
      .hbrBackground = nullptr,
      .lpszClassName = window_class_name,
      .hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(ID_APP_ICON)),
  };
  if (RegisterClassExW(&window_class) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  const HWND window = CreateWindowExW(
      0, window_class_name, window_title,
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
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
  impl_->create_child_controls();
  impl_->show_empty_prompt();
  DragAcceptFiles(window, TRUE);

  ShowWindow(window, show_command);
  UpdateWindow(window);
  return true;
}

void MainWindow::open_path(const std::filesystem::path& path) {
  impl_->open_path(path);
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
      impl_->clamp_thumbnail_scroll();
      impl_->fit_displayed_frame();
      impl_->update_child_layout();
      return 0;
    }

    case WM_PAINT: {
      PAINTSTRUCT paint{};
      BeginPaint(impl_->window, &paint);

      std::optional<core::Error> draw_error;
      bool draw_succeeded = false;
      if (impl_->renderer_ready) {
        auto overlay = impl_->make_render_overlay();
        auto draw_result = impl_->renderer.draw(impl_->transform, &overlay);
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
      if (should_flush_pending_load_metrics(
              draw_succeeded, draw_error.has_value()) &&
          impl_->pending_load_debug.has_value()) {
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
      impl_->update_preview_label();
      impl_->start_animation(std::move(loaded->animation));
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
      impl_->thumbnail_entries_dirty = true;
      impl_->retain_relevant_frames();
      impl_->prefetch_neighbors();
      impl_->refresh_thumbnail_items();
      return 0;
    }

    case WM_MOUSEWHEEL: {
      POINT cursor{
          GET_X_LPARAM(lparam),
          GET_Y_LPARAM(lparam),
      };
      if (ScreenToClient(impl_->window, &cursor) != FALSE &&
          impl_->is_point_in_thumbnail_panel({cursor.x, cursor.y})) {
        const int steps = impl_->wheel_delta.consume(
            GET_WHEEL_DELTA_WPARAM(wparam));
        static_cast<void>(impl_->scroll_thumbnail_panel(steps));
        return 0;
      }
      const int steps = impl_->wheel_delta.consume(
          GET_WHEEL_DELTA_WPARAM(wparam));
      if (steps != 0) {
        impl_->transform.zoom_by(
            static_cast<float>(std::pow(1.1, steps)));
        InvalidateRect(impl_->window, nullptr, FALSE);
      }
      return 0;
    }

    case WM_LBUTTONDOWN: {
      const Point point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (const auto toolbar_index = impl_->toolbar_hit(point)) {
        impl_->invoke_toolbar(toolbar_first_id + *toolbar_index);
        return 0;
      }
      if (impl_->begin_thumbnail_panel_resize(point)) {
        return 0;
      }
      if (const auto thumbnail_index = impl_->thumbnail_hit(point)) {
        impl_->select_thumbnail(*thumbnail_index);
        return 0;
      }
      RECT image_area = impl_->image_area_rect();
      POINT native{point.x, point.y};
      if (PtInRect(&image_area, native) != FALSE) {
        impl_->toolbar_visible = true;
        SetTimer(impl_->window, toolbar_hide_timer_id, toolbar_hide_delay_ms,
                 nullptr);
        InvalidateRect(impl_->window, nullptr, FALSE);
      }
      impl_->pan.begin(point);
      SetCapture(impl_->window);
      return 0;
    }

    case WM_MOUSEMOVE:
      if (impl_->resizing_thumbnail_panel) {
        impl_->resize_thumbnail_panel_to(
            {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        return 0;
      }
      if (impl_->is_on_thumbnail_splitter(
              {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) {
        SetCursor(LoadCursorW(nullptr, impl_->thumbnail_resize_cursor()));
        return 0;
      }
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

        case KeyAction::open:
          impl_->open_with_dialog();
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

        case KeyAction::toggle_thumbnails:
          impl_->toggle_thumbnails();
          return 0;

        case KeyAction::cycle_thumbnail_dock:
          impl_->cycle_thumbnail_dock();
          return 0;

        case KeyAction::toggle_thumbnail_preview:
          impl_->toggle_thumbnail_preview();
          return 0;

        case KeyAction::grow_thumbnails:
          impl_->grow_thumbnails();
          return 0;

        case KeyAction::shrink_thumbnails:
          impl_->shrink_thumbnails();
          return 0;

        case KeyAction::none:
          break;
      }
      break;

    case WM_SETCURSOR:
      if (LOWORD(lparam) == HTCLIENT) {
        POINT cursor{};
        if (GetCursorPos(&cursor) != FALSE &&
            ScreenToClient(impl_->window, &cursor) != FALSE &&
            impl_->is_on_thumbnail_splitter({cursor.x, cursor.y})) {
          SetCursor(LoadCursorW(nullptr, impl_->thumbnail_resize_cursor()));
          return TRUE;
        }
      }
      break;

    case WM_DROPFILES: {
      const HDROP drop = reinterpret_cast<HDROP>(wparam);
      impl_->open_first_dropped_file(drop);
      DragFinish(drop);
      return 0;
    }

    case WM_CLOSE:
      impl_->end_pan();
      impl_->shutdown_async();
      impl_->stop_animation();
      impl_->cleanup_temporary_archives();
      impl_->cleanup_gdi_resources();
      DestroyWindow(impl_->window);
      return 0;

    case WM_TIMER:
      if (wparam == animation_timer_id) {
        impl_->advance_animation();
        return 0;
      }
      if (wparam == toolbar_hide_timer_id) {
        KillTimer(impl_->window, toolbar_hide_timer_id);
        impl_->toolbar_visible = false;
        InvalidateRect(impl_->window, nullptr, FALSE);
        return 0;
      }
      break;

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
