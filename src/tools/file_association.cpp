#include <windows.h>
#include <shellapi.h>
#include <shlobj_core.h>
#include <shobjidl_core.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::wstring_view app_name = L"FlashView";
constexpr std::wstring_view viewer_exe_name = L"FlashView.exe";
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

std::optional<std::wstring> read_string_value(const std::wstring& subkey,
                                              const wchar_t* name) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_QUERY_VALUE,
                    &key) != ERROR_SUCCESS) {
    return std::nullopt;
  }

  DWORD type = 0;
  DWORD byte_count = 0;
  LONG result =
      RegQueryValueExW(key, name, nullptr, &type, nullptr, &byte_count);
  if (result != ERROR_SUCCESS || type != REG_SZ || byte_count == 0) {
    RegCloseKey(key);
    return std::nullopt;
  }

  std::wstring value(byte_count / sizeof(wchar_t), L'\0');
  result = RegQueryValueExW(
      key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()),
      &byte_count);
  RegCloseKey(key);
  if (result != ERROR_SUCCESS) {
    return std::nullopt;
  }
  while (!value.empty() && value.back() == L'\0') {
    value.pop_back();
  }
  return value;
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

bool same_string_ignore_case(const std::wstring_view left,
                             const std::wstring_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                              right.data(), static_cast<int>(right.size()),
                              TRUE) == CSTR_EQUAL;
}

std::wstring explorer_extension_key(const std::wstring_view extension) {
  return L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" +
         std::wstring(extension);
}

void delete_user_choice(const std::wstring_view extension) {
  delete_tree(explorer_extension_key(extension) + L"\\UserChoice");
}

std::optional<std::wstring> user_choice_prog_id(
    const std::wstring_view extension) {
  return read_string_value(explorer_extension_key(extension) + L"\\UserChoice",
                           L"ProgId");
}

bool extension_default_is_flashview(const std::wstring_view extension) {
  const auto extension_default =
      read_string_value(L"Software\\Classes\\" + std::wstring(extension),
                        nullptr);
  return extension_default.has_value() && *extension_default == prog_id;
}

void delete_user_choice_if_flashview(const std::wstring_view extension) {
  const std::wstring user_choice_key =
      explorer_extension_key(extension) + L"\\UserChoice";
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

void delete_explorer_open_with_list_entries(
    const std::wstring_view extension) {
  const std::wstring open_with_list_key =
      explorer_extension_key(extension) + L"\\OpenWithList";
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, open_with_list_key.c_str(), 0,
                    KEY_QUERY_VALUE | KEY_SET_VALUE,
                    &key) != ERROR_SUCCESS) {
    return;
  }

  std::vector<std::wstring> value_names_to_delete;
  std::wstring mru_list;
  DWORD index = 0;
  while (true) {
    wchar_t value_name[256]{};
    DWORD value_name_size = static_cast<DWORD>(std::size(value_name));
    wchar_t data[512]{};
    DWORD data_size = sizeof(data);
    DWORD type = 0;
    const LONG result =
        RegEnumValueW(key, index, value_name, &value_name_size, nullptr, &type,
                      reinterpret_cast<BYTE*>(data), &data_size);
    if (result == ERROR_NO_MORE_ITEMS) {
      break;
    }
    ++index;
    if (result != ERROR_SUCCESS) {
      continue;
    }

    const std::wstring_view name(value_name, value_name_size);
    if (type == REG_SZ && name == L"MRUList") {
      mru_list = data;
      continue;
    }
    if (type == REG_SZ && same_string_ignore_case(data, viewer_exe_name)) {
      value_names_to_delete.emplace_back(name);
    }
  }

  for (const auto& value_name : value_names_to_delete) {
    RegDeleteValueW(key, value_name.c_str());
  }

  if (!mru_list.empty() && !value_names_to_delete.empty()) {
    for (const auto& value_name : value_names_to_delete) {
      if (value_name.size() == 1) {
        mru_list.erase(std::remove(mru_list.begin(), mru_list.end(),
                                   value_name.front()),
                       mru_list.end());
      }
    }
    set_string_value(key, L"MRUList", mru_list);
  }

  RegCloseKey(key);
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

struct AssociationResult {
  bool registry_write_ok = true;
  std::vector<std::wstring> blocked_extensions;
};

bool set_default_with_windows(IApplicationAssociationRegistration* registration,
                              const std::wstring_view extension) {
  if (registration == nullptr) {
    return false;
  }
  return SUCCEEDED(registration->SetAppAsDefault(
      std::wstring(app_name).c_str(), std::wstring(extension).c_str(),
      AT_FILEEXTENSION));
}

std::optional<bool> effective_default_is_flashview(
    IApplicationAssociationRegistration* registration,
    const std::wstring_view extension) {
  if (registration == nullptr) {
    return std::nullopt;
  }

  LPWSTR current_default = nullptr;
  const HRESULT result = registration->QueryCurrentDefault(
      std::wstring(extension).c_str(), AT_FILEEXTENSION, AL_EFFECTIVE,
      &current_default);
  if (FAILED(result) || current_default == nullptr) {
    return std::nullopt;
  }

  const std::wstring_view effective_default(current_default);
  const bool is_flashview =
      same_string_ignore_case(effective_default, prog_id) ||
      same_string_ignore_case(
          effective_default,
          L"Applications\\" + std::wstring(viewer_exe_name));
  CoTaskMemFree(current_default);
  return is_flashview;
}

AssociationResult register_associations(
    const std::filesystem::path& viewer_path) {
  const std::wstring exe = viewer_path.wstring();
  const std::wstring command = quote(exe) + L" \"%1\"";
  const std::wstring classes = L"Software\\Classes\\";
  const std::wstring prog_key = classes + std::wstring(prog_id);
  const std::wstring application_key =
      classes + L"Applications\\" + std::wstring(viewer_exe_name);
  const std::wstring capabilities_key = application_key + L"\\Capabilities";
  const std::wstring context_menu_key =
      classes + L"SystemFileAssociations\\image\\shell\\FlashView";
  const std::wstring app_paths_key =
      L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" +
      std::wstring(viewer_exe_name);

  const HRESULT com_init =
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool should_uninitialize_com = SUCCEEDED(com_init);
  IApplicationAssociationRegistration* registration = nullptr;
  if (SUCCEEDED(com_init) || com_init == RPC_E_CHANGED_MODE) {
    CoCreateInstance(CLSID_ApplicationAssociationRegistration, nullptr,
                     CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&registration));
  }

  AssociationResult result;
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
      L"Software\\Classes\\Applications\\FlashView.exe\\Capabilities");
  ok &= write_key_default(context_menu_key, L"Open with FlashView");
  ok &= write_key_value(context_menu_key, L"Icon", quote(exe) + L",-0");
  ok &= write_key_default(context_menu_key + L"\\command", command);

  for (const auto extension : image_extensions) {
    const std::wstring extension_string(extension);
    const std::wstring extension_key = classes + extension_string;
    delete_user_choice(extension);
    ok &= write_key_default(extension_key, std::wstring(prog_id));
    ok &= write_key_default(extension_key + L"\\OpenWithList\\" +
                                std::wstring(viewer_exe_name),
                            L"");
    ok &= write_empty_binary_value(extension_key + L"\\OpenWithProgids",
                                   prog_id);
    ok &= write_empty_binary_value(explorer_extension_key(extension) +
                                       L"\\OpenWithProgids",
                                   prog_id);
    ok &= write_key_value(capabilities_key + L"\\FileAssociations",
                          extension_string.c_str(), std::wstring(prog_id));
    ok &= write_key_value(application_key + L"\\SupportedTypes",
                          extension_string.c_str(), L"");

    set_default_with_windows(registration, extension);
    const std::optional<bool> effective_default =
        effective_default_is_flashview(registration, extension);
    const auto user_choice = user_choice_prog_id(extension);
    if ((effective_default.has_value() && !*effective_default) ||
        (!effective_default.has_value() &&
         ((user_choice.has_value() && *user_choice != prog_id) ||
          (!user_choice.has_value() &&
           !extension_default_is_flashview(extension))))) {
      result.blocked_extensions.push_back(extension_string);
    }
  }

  if (registration != nullptr) {
    registration->Release();
  }
  if (should_uninitialize_com) {
    CoUninitialize();
  }

  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  result.registry_write_ok = ok;
  return result;
}

void unregister_associations() {
  const std::wstring classes = L"Software\\Classes\\";
  const std::wstring application_key =
      classes + L"Applications\\" + std::wstring(viewer_exe_name);
  const std::wstring context_menu_key =
      classes + L"SystemFileAssociations\\image\\shell\\FlashView";

  for (const auto extension : image_extensions) {
    const std::wstring extension_string(extension);
    const std::wstring extension_key = classes + extension_string;
    clear_extension_default_if_flashview(extension_key);
    delete_user_choice_if_flashview(extension);
    delete_tree(extension_key + L"\\OpenWithList\\" +
                std::wstring(viewer_exe_name));
    delete_value(extension_key + L"\\OpenWithProgids", prog_id);
    delete_explorer_open_with_list_entries(extension);
    delete_value(explorer_extension_key(extension) + L"\\OpenWithProgids", prog_id);
    delete_value(
        L"Software\\Microsoft\\Windows\\CurrentVersion\\ApplicationAssociationToasts",
        std::wstring(prog_id) + L"_" + extension_string);
    delete_value(
        L"Software\\Microsoft\\Windows\\CurrentVersion\\ApplicationAssociationToasts",
        L"Applications\\" + std::wstring(viewer_exe_name) + L"_" +
            extension_string);
  }

  delete_tree(classes + std::wstring(prog_id));
  delete_tree(application_key);
  delete_tree(context_menu_key);
  delete_tree(L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" +
              std::wstring(viewer_exe_name));
  delete_value(L"Software\\RegisteredApplications", app_name);
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

void show_message(const std::wstring& text, UINT icon = MB_ICONINFORMATION) {
  MessageBoxW(nullptr, text.c_str(), L"FlashView", MB_OK | icon);
}

std::wstring blocked_extension_message(const AssociationResult& result) {
  std::wstringstream message;
  message << L"FlashView was added to Open with, but Windows did not allow "
             L"direct double-click takeover for:\n";
  for (const auto& extension : result.blocked_extensions) {
    message << L"  " << extension << L"\n";
  }
  message << L"\nThis is Windows 10/11 default-app protection. "
             L"The image right-click menu was still added.\n\n";
  message << L"For double-click default opening, choose FlashView once in "
             L"Windows Settings > Apps > Default apps.";
  return message.str();
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
        L"FlashView.exe was not found.\n\n"
        L"Please keep this program in the same folder as FlashView.exe.",
        MB_ICONERROR);
    return 1;
  }

  const AssociationResult association_result =
      register_associations(*viewer_path);
  if (!association_result.registry_write_ok) {
    show_message(
        L"FlashView could not finish registering file associations.\n\n"
        L"Please try again, or move FlashView to a writable folder.",
        MB_ICONERROR);
    return 1;
  }

  if (!association_result.blocked_extensions.empty()) {
    show_message(blocked_extension_message(association_result),
                 MB_ICONWARNING);
    return 2;
  }

  MessageBoxW(nullptr,
              L"FlashView is now associated with supported image files.\n\n"
              L"No archive formats were associated.",
              L"FlashView", MB_OK | MB_ICONINFORMATION);
  return 0;
#endif
}
