#include "app/app.h"

#include <objbase.h>
#include <shellapi.h>

#include "app/main_window.h"

namespace viewer::app {
namespace {

constexpr wchar_t startup_error_title[] = L"FlashView";

class ComApartment {
 public:
  ComApartment() noexcept
      : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)),
        owns_initialization_(result_ == S_OK || result_ == S_FALSE) {}

  ~ComApartment() {
    if (owns_initialization_) {
      CoUninitialize();
    }
  }

  ComApartment(const ComApartment&) = delete;
  ComApartment& operator=(const ComApartment&) = delete;

  [[nodiscard]] bool is_usable() const noexcept {
    return owns_initialization_ || result_ == RPC_E_CHANGED_MODE;
  }

 private:
  HRESULT result_;
  bool owns_initialization_;
};

}  // namespace

int run(HINSTANCE instance, int show_command) {
  static_cast<void>(SetProcessDpiAwarenessContext(
      DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));

  const ComApartment com_apartment;
  if (!com_apartment.is_usable()) {
    MessageBoxW(nullptr, L"Unable to initialize COM.", startup_error_title,
                MB_OK | MB_ICONERROR);
    return 1;
  }

  MainWindow window;
  if (!window.create(instance, show_command)) {
    MessageBoxW(
        nullptr,
        L"Unable to start FlashView. Direct3D/Direct2D initialization failed.",
        startup_error_title, MB_OK | MB_ICONERROR);
    return 1;
  }

  int argument_count = 0;
  LPWSTR* arguments =
      CommandLineToArgvW(GetCommandLineW(), &argument_count);
  if (arguments != nullptr) {
    if (argument_count > 1) {
      window.open_path(arguments[1]);
    }
    LocalFree(arguments);
  }

  MSG message{};
  while (true) {
    const BOOL message_result = GetMessageW(&message, nullptr, 0, 0);
    if (message_result == -1) {
      return 1;
    }
    if (message_result == 0) {
      return static_cast<int>(message.wParam);
    }

    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

}  // namespace viewer::app
