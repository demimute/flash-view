#pragma once

#include <Windows.h>

#include <filesystem>
#include <memory>

namespace viewer::app {

class MainWindow {
 public:
  MainWindow();
  ~MainWindow();
  MainWindow(const MainWindow&) = delete;
  MainWindow& operator=(const MainWindow&) = delete;

  bool create(HINSTANCE instance, int show_command);
  void open_path(const std::filesystem::path& path);

 private:
  static LRESULT CALLBACK window_proc(
      HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace viewer::app
