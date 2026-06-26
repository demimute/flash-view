#include <windows.h>
#include <shellapi.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr std::wstring_view app_name = L"FlashView";
constexpr std::wstring_view viewer_exe_name = L"fast_viewer.exe";
constexpr std::wstring_view prog_id = L"FlashView.Image";
constexpr std::array<std::wstring_view, 13> image_extensions{
    L".jpg",  L".jpeg", L".png", L".bmp",  L".gif",
    L".tif",  L".tiff", L".ico", L".webp", L".heic",
    L".heif", L".avif", L".jxl",
};

std::wstring quote(const std::wstring& value) {
  return L"\"" + value + L"\"";
}

std::optional<std::filesystem::path> module_directory() {
  std::wstring buffer(MAX_PATH, L'\0');
  DWORD size = 0;
  while (true) {
    size = GetModuleFileNameW(nullptr, buffer.data(),
                              static_cast<DWORD>(buffer.size()));
    if (size == 0) {
      return std::nullopt;
    }
    if (size < buffer.size() - 1) {
      buffer.resize(size);
      return std::filesystem::path(buffer).parent_path();
    }
    buffer.resize(buffer.size() * 2);
  }
}

std::optional<std::filesystem::path> viewer_path_from_arguments() {
  int argument_count = 0;
  LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
  if (arguments == nullptr) {
    return std::nullopt;
  }

  std::optional<std::filesystem::path> result;
  if (argument_count >= 2 && arguments[1] != nullptr &&
      arguments[1][0] != L'\0') {
    result = std::filesystem::path(arguments[1]);
  }
  LocalFree(arguments);
  return result;
}

std::optional<std::filesystem::path> resolve_viewer_path() {
  if (auto argument_path = viewer_path_from_arguments();
      argument_path.has_value()) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(*argument_path, error);
    if (!error && std::filesystem::exists(absolute, error)) {
      return absolute;
    }
  }

  const auto directory = module_directory();
  if (!directory.has_value()) {
    return std::nullopt;
  }

  std::error_code error;
  const auto side_by_side = *directory / viewer_exe_name;
  if (std::filesystem::exists(side_by_side, error)) {
    return side_by_side;
  }

  return std::nullopt;
}

bool create_key(HKEY root, const std::wstring& subkey, HKEY* key) {
  return RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_WRITE,
                         nullptr, key, nullptr) == ERROR_SUCCESS;
}

bool set_string_value(HKEY key, const wchar_t* name,
                      const std::wstring& value) {
  return RegSetValueExW(
             key, name, 0, REG_SZ,
             reinterpret_cast<const BYTE*>(value.c_str()),
             static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) ==
         ERROR_SUCCESS;
}

bool set_empty_binary_value(HKEY key, const std::wstring_view name) {
  return RegSetValueExW(key, std::wstring(name).c_str(), 0, REG_BINARY, nullptr,
                        0) == ERROR_SUCCESS;
}

bool write_key_default(const std::wstring& subkey, const std::wstring& value) {
  HKEY key = nullptr;
  if (!create_key(HKEY_CURRENT_USER, subkey, &key)) {
    return false;
  }
  const bool ok = set_string_value(key, nullptr, value);
  RegCloseKey(key);
  return ok;
}

bool write_key_value(const std::wstring& subkey, const wchar_t* name,
                     const std::wstring& value) {
  HKEY key = nullptr;
  if (!create_key(HKEY_CURRENT_USER, subkey, &key)) {
    return false;
  }
  const bool ok = set_string_value(key, name, value);
  RegCloseKey(key);
  return ok;
}

bool write_empty_binary_value(const std::wstring& subkey,
                              const std::wstring_view name) {
  HKEY key = nullptr;
  if (!create_key(HKEY_CURRENT_USER, subkey, &key)) {
    return false;
  }
  const bool ok = set_empty_binary_value(key, name);
  RegCloseKey(key);
  return ok;
}

void delete_tree(const std::wstring& subkey) {
  RegDeleteTreeW(HKEY_CURRENT_USER, subkey.c_str());
}

void delete_value(const std::wstring& subkey, const std::wstring_view name) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_SET_VALUE,
                    &key) != ERROR_SUCCESS) {
    return;
  }
  RegDeleteValueW(key, std::wstring(name).c_str());
  RegCloseKey(key);
}

void delete_user_choice(const std::wstring_view extension) {
  delete_tree(
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" +
      std::wstring(extension) + L"\\UserChoice");
}

void delete_user_choice_if_flashview(const std::wstring_view extension) {
  const std::wstring user_choice_key =
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" +
      std::wstring(extension) + L"\\UserChoice";
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, user_choice_key.c_str(), 0,
                    KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return;
  }

  wchar_t value[256]{};
  DWORD value_size = sizeof(value);
  DWORD type = 0;
  const bool is_flashview =
      RegQueryValueExW(key, L"ProgId", nullptr, &type,
                       reinterpret_cast<BYTE*>(value),
                       &value_size) == ERROR_SUCCESS &&
      type == REG_SZ && std::wstring_view(value) == prog_id;
  RegCloseKey(key);

  if (is_flashview) {
    delete_tree(user_choice_key);
  }
}

void clear_extension_default_if_flashview(const std::wstring& extension_key) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, extension_key.c_str(), 0,
                    KEY_QUERY_VALUE | KEY_SET_VALUE,
                    &key) != ERROR_SUCCESS) {
    return;
  }

  wchar_t value[256]{};
  DWORD value_size = sizeof(value);
  DWORD type = 0;
  if (RegQueryValueExW(key, nullptr, nullptr, &type,
                       reinterpret_cast<BYTE*>(value),
                       &value_size) == ERROR_SUCCESS &&
      type == REG_SZ && std::wstring_view(value) == prog_id) {
    set_string_value(key, nullptr, L"");
  }
  RegCloseKey(key);
}

bool register_associations(const std::filesystem::path& viewer_path) {
  const std::wstring exe = viewer_path.wstring();
  const std::wstring command = quote(exe) + L" \"%1\"";
  const std::wstring classes = L"Software\\Classes\\";
  const std::wstring prog_key = classes + std::wstring(prog_id);
  const std::wstring application_key =
      classes + L"Applications\\" + std::wstring(viewer_exe_name);
  const std::wstring capabilities_key = application_key + L"\\Capabilities";
  const std::wstring app_paths_key =
      L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" +
      std::wstring(viewer_exe_name);

  bool ok = true;
  ok &= write_key_default(app_paths_key, exe);
  ok &= write_key_default(prog_key, L"FlashView image");
  ok &= write_key_default(prog_key + L"\\DefaultIcon", quote(exe) + L",-102");
  ok &= write_key_default(prog_key + L"\\shell\\open\\command", command);

  ok &= write_key_value(application_key, L"FriendlyAppName",
                        std::wstring(app_name));
  ok &= write_key_default(application_key + L"\\DefaultIcon",
                          quote(exe) + L",-0");
  ok &= write_key_default(application_key + L"\\shell\\open\\command",
                          command);
  ok &= write_key_value(capabilities_key, L"ApplicationName",
                        std::wstring(app_name));
  ok &= write_key_value(capabilities_key, L"ApplicationDescription",
                        L"Fast portable image viewer");
  ok &= write_key_value(
      L"Software\\RegisteredApplications", std::wstring(app_name).c_str(),
      L"Software\\Classes\\Applications\\fast_viewer.exe\\Capabilities");

  for (const auto extension : image_extensions) {
    const std::wstring extension_string(extension);
    const std::wstring extension_key = classes + extension_string;
    delete_user_choice(extension);
    ok &= write_key_default(extension_key, std::wstring(prog_id));
    ok &= write_empty_binary_value(extension_key + L"\\OpenWithProgids",
                                   prog_id);
    ok &= write_key_value(capabilities_key + L"\\FileAssociations",
                          extension_string.c_str(), std::wstring(prog_id));
    ok &= write_key_value(application_key + L"\\SupportedTypes",
                          extension_string.c_str(), L"");
  }

  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return ok;
}

void unregister_associations() {
  const std::wstring classes = L"Software\\Classes\\";
  const std::wstring application_key =
      classes + L"Applications\\" + std::wstring(viewer_exe_name);

  for (const auto extension : image_extensions) {
    const std::wstring extension_string(extension);
    const std::wstring extension_key = classes + extension_string;
    clear_extension_default_if_flashview(extension_key);
    delete_user_choice_if_flashview(extension);
    delete_value(extension_key + L"\\OpenWithProgids", prog_id);
  }

  delete_tree(classes + std::wstring(prog_id));
  delete_tree(application_key);
  delete_tree(L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" +
              std::wstring(viewer_exe_name));
  delete_value(L"Software\\RegisteredApplications", app_name);
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

void show_message(const std::wstring& text, UINT icon = MB_ICONINFORMATION) {
  MessageBoxW(nullptr, text.c_str(), L"FlashView", MB_OK | icon);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
#if defined(FLASHVIEW_UNASSOCIATE)
  unregister_associations();
  show_message(L"FlashView file associations have been removed.");
  return 0;
#else
  const auto viewer_path = resolve_viewer_path();
  if (!viewer_path.has_value()) {
    show_message(
        L"fast_viewer.exe was not found.\n\n"
        L"Please keep this program in the same folder as fast_viewer.exe.",
        MB_ICONERROR);
    return 1;
  }

  if (!register_associations(*viewer_path)) {
    show_message(
        L"FlashView could not finish registering file associations.\n\n"
        L"Please try again, or move FlashView to a writable folder.",
        MB_ICONERROR);
    return 1;
  }

  MessageBoxW(nullptr,
      L"FlashView is now associated with supported image files.\n\n"
      L"No archive formats were associated.",
      L"FlashView", MB_OK | MB_ICONINFORMATION);
  return 0;
#endif
}
