#include <Windows.h>

#include "app/app.h"

int WINAPI wWinMain(
    HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
  return viewer::app::run(instance, show_command);
}
