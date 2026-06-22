# Fast Image Viewer Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a Windows x64 native executable that opens JPEG/PNG/BMP images asynchronously, renders them through Direct3D 11/Direct2D, scans the containing directory using natural ordering, and supports responsive previous/next navigation.

**Architecture:** Keep platform-neutral navigation, format probing, task scheduling, and image-result types in `viewer_core`; isolate Windows file decoding in `viewer_wic`, GPU drawing in `viewer_render`, and Win32 lifecycle/input in `viewer_app`. The UI thread only creates windows, handles input, submits work, uploads completed frames, and presents; scanning and decoding run on cancellable priority workers.

**Tech Stack:** C++20, CMake 3.28+, MSVC 2022, Windows 10 SDK, Win32, WIC, Direct3D 11, Direct2D 1.1, DirectWrite, GoogleTest through vcpkg manifest, GitHub Actions Windows runner.

---

## Scope and phase boundaries

This plan implements the first independently useful slice of the approved design:

- Native Win32 window and DPI awareness.
- Direct3D 11/Direct2D rendering.
- WIC decoding for JPEG, PNG, and BMP.
- Content-based format probing.
- Asynchronous decoding and stale-result suppression.
- Directory scan, natural sort, previous/next navigation.
- Fit-to-window, 1:1, wheel zoom, pan, and view-only clockwise rotation.
- Lightweight error overlay and basic timing instrumentation.

The following approved requirements remain separate plans so each phase stays testable:

1. **Phase 2 — Thumbnail workspace:** virtualized grid, preview pane, four-side docking, adjustable split, memory/disk cache, SQLite index.
2. **Phase 3 — Format expansion:** GIF/APNG/animated WebP, libjpeg-turbo, WebP, AVIF, HEIC, RAW, PSD, decoder qualification harness.
3. **Phase 4 — Archives:** 7-Zip SDK, UnRAR, encrypted/split/solid archives, archive navigation and thumbnail generation.
4. **Phase 5 — Color and hardening:** ICC monitor conversion, memory-pressure policy, WARP recovery, fuzzing, hostile-input limits, performance corpus and release packaging.

Do not pull Phase 2–5 functionality into this plan.

## Required development environment

- Windows 10 22H2 or Windows 11 x64.
- Visual Studio 2022 with “Desktop development with C++”.
- Windows 10/11 SDK.
- CMake 3.28 or newer on `PATH`.
- vcpkg available through `VCPKG_ROOT`.
- Ninja is optional; commands below use Visual Studio generator for predictability.

Configure command used throughout:

```powershell
cmake --preset windows-msvc-debug
```

Build command used throughout:

```powershell
cmake --build --preset windows-msvc-debug
```

Test command used throughout:

```powershell
ctest --preset windows-msvc-debug --output-on-failure
```

## Planned file structure

```text
.
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── cmake/
│   └── CompilerWarnings.cmake
├── include/viewer/
│   ├── core/
│   │   ├── cancellation.h
│   │   ├── directory_navigator.h
│   │   ├── format_probe.h
│   │   ├── image_frame.h
│   │   ├── load_request.h
│   │   ├── natural_sort.h
│   │   ├── priority_executor.h
│   │   ├── result.h
│   │   └── view_transform.h
│   ├── platform/
│   │   └── wic_decoder.h
│   └── render/
│       └── d3d_renderer.h
├── src/
│   ├── app/
│   │   ├── app.cpp
│   │   ├── app.h
│   │   ├── main_window.cpp
│   │   ├── main_window.h
│   │   ├── resource.h
│   │   └── winmain.cpp
│   ├── core/
│   │   ├── directory_navigator.cpp
│   │   ├── format_probe.cpp
│   │   ├── natural_sort.cpp
│   │   ├── priority_executor.cpp
│   │   └── view_transform.cpp
│   ├── platform/
│   │   └── wic_decoder.cpp
│   └── render/
│       └── d3d_renderer.cpp
├── tests/
│   ├── CMakeLists.txt
│   ├── core/
│   │   ├── directory_navigator_test.cpp
│   │   ├── format_probe_test.cpp
│   │   ├── natural_sort_test.cpp
│   │   ├── priority_executor_test.cpp
│   │   └── view_transform_test.cpp
│   ├── fixtures/
│   │   ├── 1x1.bmp
│   │   ├── 1x1.png
│   │   └── corrupt.png
│   └── platform/
│       └── wic_decoder_test.cpp
├── tools/
│   └── make_test_fixtures.ps1
└── .github/workflows/
    └── windows.yml
```

Each header defines one public responsibility. App files may depend on all libraries; core must not include Windows headers.

### Task 1: Bootstrap a reproducible Windows C++ project

**Files:**
- Create: `CMakeLists.txt`
- Create: `CMakePresets.json`
- Create: `vcpkg.json`
- Create: `cmake/CompilerWarnings.cmake`
- Create: `tests/CMakeLists.txt`
- Create: `tests/core/smoke_test.cpp`

- [ ] **Step 1: Add a failing smoke test target reference**

Create `tests/core/smoke_test.cpp`:

```cpp
#include <gtest/gtest.h>

TEST(ProjectSmoke, TestRunnerStarts) {
    EXPECT_EQ(2 + 2, 4);
}
```

Create `tests/CMakeLists.txt`:

```cmake
find_package(GTest CONFIG REQUIRED)

add_executable(viewer_tests
    core/smoke_test.cpp
)

target_link_libraries(viewer_tests PRIVATE GTest::gtest_main)
target_compile_features(viewer_tests PRIVATE cxx_std_20)
viewer_enable_warnings(viewer_tests)

include(GoogleTest)
gtest_discover_tests(viewer_tests)
```

- [ ] **Step 2: Run configure and verify it fails before project files exist**

Run:

```powershell
cmake --preset windows-msvc-debug
```

Expected: failure because `CMakePresets.json` or the top-level CMake project does not exist.

- [ ] **Step 3: Add pinned dependencies and build configuration**

Create `vcpkg.json`:

```json
{
  "name": "fast-image-viewer",
  "version-string": "0.1.0",
  "dependencies": [
    "gtest"
  ]
}
```

Populate and commit the baseline from the exact vcpkg checkout used by the
project:

```powershell
& "$env:VCPKG_ROOT\vcpkg.exe" x-update-baseline --add-initial-baseline
```

Expected: `vcpkg.json` gains a 40-character `builtin-baseline`. Verify it:

```powershell
$manifest = Get-Content .\vcpkg.json | ConvertFrom-Json
if ($manifest.'builtin-baseline' -notmatch '^[0-9a-f]{40}$') {
  throw "vcpkg baseline was not pinned"
}
```

Create `CMakePresets.json`:

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "windows-msvc-debug",
      "displayName": "Windows MSVC Debug",
      "generator": "Visual Studio 17 2022",
      "architecture": "x64",
      "binaryDir": "${sourceDir}/build/windows-msvc-debug",
      "cacheVariables": {
        "CMAKE_CXX_STANDARD": "20",
        "CMAKE_CXX_STANDARD_REQUIRED": "ON",
        "CMAKE_TOOLCHAIN_FILE": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
        "BUILD_TESTING": "ON"
      }
    },
    {
      "name": "windows-msvc-release",
      "inherits": "windows-msvc-debug",
      "displayName": "Windows MSVC Release",
      "binaryDir": "${sourceDir}/build/windows-msvc-release",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release",
        "BUILD_TESTING": "OFF"
      }
    }
  ],
  "buildPresets": [
    {
      "name": "windows-msvc-debug",
      "configurePreset": "windows-msvc-debug",
      "configuration": "Debug"
    },
    {
      "name": "windows-msvc-release",
      "configurePreset": "windows-msvc-release",
      "configuration": "Release"
    }
  ],
  "testPresets": [
    {
      "name": "windows-msvc-debug",
      "configurePreset": "windows-msvc-debug",
      "configuration": "Debug"
    }
  ]
}
```

Create `cmake/CompilerWarnings.cmake`:

```cmake
function(viewer_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()
```

Create `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.28)
project(FastImageViewer VERSION 0.1.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(CTest)

add_library(viewer_core STATIC)
target_compile_features(viewer_core PUBLIC cxx_std_20)
target_include_directories(viewer_core PUBLIC include)
viewer_enable_warnings(viewer_core)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
```

- [ ] **Step 4: Configure, build, and run the smoke test**

Run:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug --output-on-failure
```

Expected: configure succeeds, `viewer_tests.exe` builds, and one test passes.

- [ ] **Step 5: Commit the bootstrap**

```powershell
git add CMakeLists.txt CMakePresets.json vcpkg.json cmake tests
git commit -m "build: bootstrap Windows C++ project"
```

### Task 2: Define shared result, cancellation, request, and image types

**Files:**
- Create: `include/viewer/core/result.h`
- Create: `include/viewer/core/cancellation.h`
- Create: `include/viewer/core/load_request.h`
- Create: `include/viewer/core/image_frame.h`
- Create: `tests/core/image_types_test.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write tests for immutable request identity and bounded image size**

Create `tests/core/image_types_test.cpp`:

```cpp
#include <cstddef>
#include <filesystem>
#include <gtest/gtest.h>

#include "viewer/core/cancellation.h"
#include "viewer/core/image_frame.h"
#include "viewer/core/load_request.h"

using namespace viewer::core;

TEST(ImageTypes, RequestCarriesMonotonicGeneration) {
    LoadRequest request{.path = L"C:\\images\\a.jpg", .generation = 42};
    EXPECT_EQ(request.generation, 42u);
    EXPECT_EQ(request.path.filename(), L"a.jpg");
}

TEST(ImageTypes, CancellationTokenObservesCancellation) {
    CancellationSource source;
    EXPECT_FALSE(source.token().is_cancelled());
    source.cancel();
    EXPECT_TRUE(source.token().is_cancelled());
}

TEST(ImageTypes, RejectsImpossibleAllocation) {
    auto result = ImageFrame::allocate_bgra8(1'000'000, 1'000'000, 256_MiB);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::resource_limit);
}

TEST(ImageTypes, AllocatesSmallBgraFrame) {
    auto result = ImageFrame::allocate_bgra8(2, 3, 1_MiB);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().stride, 8u);
    EXPECT_EQ(result.value().pixels.size(), 24u);
}
```

Add `core/image_types_test.cpp` to `viewer_tests` in `tests/CMakeLists.txt`.

- [ ] **Step 2: Build and verify the test fails**

Run:

```powershell
cmake --build --preset windows-msvc-debug
```

Expected: compile failure because the four headers and their types do not exist.

- [ ] **Step 3: Implement the minimal shared types**

Create `include/viewer/core/result.h`:

```cpp
#pragma once

#include <string>
#include <utility>
#include <variant>

namespace viewer::core {

enum class ErrorCode {
    cancelled,
    invalid_format,
    unsupported_format,
    io_error,
    decode_error,
    resource_limit,
    platform_error
};

struct Error {
    ErrorCode code;
    std::wstring message;
};

template <typename T>
class Result {
public:
    static Result success(T value) { return Result(std::move(value)); }
    static Result failure(Error error) { return Result(std::move(error)); }

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<T>(storage_);
    }

    [[nodiscard]] T& value() { return std::get<T>(storage_); }
    [[nodiscard]] const T& value() const { return std::get<T>(storage_); }
    [[nodiscard]] const Error& error() const { return std::get<Error>(storage_); }

private:
    explicit Result(T value) : storage_(std::move(value)) {}
    explicit Result(Error error) : storage_(std::move(error)) {}
    std::variant<T, Error> storage_;
};

}  // namespace viewer::core
```

Create `include/viewer/core/cancellation.h`:

```cpp
#pragma once

#include <atomic>
#include <memory>

namespace viewer::core {

class CancellationToken {
public:
    CancellationToken() = default;
    explicit CancellationToken(std::shared_ptr<std::atomic_bool> state)
        : state_(std::move(state)) {}

    [[nodiscard]] bool is_cancelled() const noexcept {
        return state_ && state_->load(std::memory_order_relaxed);
    }

private:
    std::shared_ptr<std::atomic_bool> state_;
};

class CancellationSource {
public:
    CancellationSource() : state_(std::make_shared<std::atomic_bool>(false)) {}
    [[nodiscard]] CancellationToken token() const { return CancellationToken(state_); }
    void cancel() noexcept { state_->store(true, std::memory_order_relaxed); }

private:
    std::shared_ptr<std::atomic_bool> state_;
};

}  // namespace viewer::core
```

Create `include/viewer/core/load_request.h`:

```cpp
#pragma once

#include <cstdint>
#include <filesystem>

namespace viewer::core {

struct LoadRequest {
    std::filesystem::path path;
    std::uint64_t generation = 0;
};

}  // namespace viewer::core
```

Create `include/viewer/core/image_frame.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "viewer/core/result.h"

namespace viewer::core {

constexpr std::size_t operator""_MiB(unsigned long long value) {
    return static_cast<std::size_t>(value) * 1024u * 1024u;
}

struct ImageFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::vector<std::byte> pixels;

    static Result<ImageFrame> allocate_bgra8(
        std::uint32_t width,
        std::uint32_t height,
        std::size_t byte_budget) {
        constexpr std::size_t bytes_per_pixel = 4;
        if (width == 0 || height == 0) {
            return Result<ImageFrame>::failure(
                {ErrorCode::decode_error, L"Image dimensions must be non-zero."});
        }
        if (width > std::numeric_limits<std::uint32_t>::max() / bytes_per_pixel) {
            return Result<ImageFrame>::failure(
                {ErrorCode::resource_limit, L"Image row is too large."});
        }
        const auto stride = static_cast<std::size_t>(width) * bytes_per_pixel;
        if (height > byte_budget / stride) {
            return Result<ImageFrame>::failure(
                {ErrorCode::resource_limit, L"Image exceeds the decode budget."});
        }
        ImageFrame frame;
        frame.width = width;
        frame.height = height;
        frame.stride = static_cast<std::uint32_t>(stride);
        frame.pixels.resize(stride * height);
        return Result<ImageFrame>::success(std::move(frame));
    }
};

}  // namespace viewer::core
```

- [ ] **Step 4: Run the focused test**

Run:

```powershell
cmake --build --preset windows-msvc-debug
ctest --test-dir build/windows-msvc-debug -C Debug -R ImageTypes --output-on-failure
```

Expected: all four `ImageTypes` tests pass.

- [ ] **Step 5: Commit the shared contracts**

```powershell
git add include/viewer/core tests
git commit -m "feat: add cancellable image load contracts"
```

### Task 3: Implement content-based format probing

**Files:**
- Create: `include/viewer/core/format_probe.h`
- Create: `src/core/format_probe.cpp`
- Create: `tests/core/format_probe_test.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing signature tests**

Create `tests/core/format_probe_test.cpp`:

```cpp
#include <array>
#include <cstddef>
#include <gtest/gtest.h>

#include "viewer/core/format_probe.h"

using viewer::core::ImageFormat;
using viewer::core::probe_format;

TEST(FormatProbe, DetectsJpegByMagic) {
    const std::array<std::byte, 4> bytes{
        std::byte{0xFF}, std::byte{0xD8}, std::byte{0xFF}, std::byte{0xE0}};
    EXPECT_EQ(probe_format(bytes), ImageFormat::jpeg);
}

TEST(FormatProbe, DetectsPngByMagic) {
    const std::array<std::byte, 8> bytes{
        std::byte{0x89}, std::byte{0x50}, std::byte{0x4E}, std::byte{0x47},
        std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A}};
    EXPECT_EQ(probe_format(bytes), ImageFormat::png);
}

TEST(FormatProbe, DetectsBmpByMagic) {
    const std::array<std::byte, 2> bytes{std::byte{'B'}, std::byte{'M'}};
    EXPECT_EQ(probe_format(bytes), ImageFormat::bmp);
}

TEST(FormatProbe, RejectsUnknownBytes) {
    const std::array<std::byte, 3> bytes{
        std::byte{0x00}, std::byte{0x01}, std::byte{0x02}};
    EXPECT_EQ(probe_format(bytes), ImageFormat::unknown);
}
```

Add this file to `viewer_tests`.

- [ ] **Step 2: Build and verify missing API failure**

Run:

```powershell
cmake --build --preset windows-msvc-debug
```

Expected: compile failure because `format_probe.h` does not exist.

- [ ] **Step 3: Implement the probe**

Create `include/viewer/core/format_probe.h`:

```cpp
#pragma once

#include <cstddef>
#include <span>

namespace viewer::core {

enum class ImageFormat {
    unknown,
    jpeg,
    png,
    bmp
};

[[nodiscard]] ImageFormat probe_format(std::span<const std::byte> bytes) noexcept;

}  // namespace viewer::core
```

Create `src/core/format_probe.cpp`:

```cpp
#include "viewer/core/format_probe.h"

#include <array>
#include <algorithm>

namespace viewer::core {
namespace {

template <std::size_t N>
bool begins_with(std::span<const std::byte> bytes,
                 const std::array<std::byte, N>& signature) {
    return bytes.size() >= N &&
           std::equal(signature.begin(), signature.end(), bytes.begin());
}

}  // namespace

ImageFormat probe_format(std::span<const std::byte> bytes) noexcept {
    constexpr std::array jpeg{
        std::byte{0xFF}, std::byte{0xD8}, std::byte{0xFF}};
    constexpr std::array png{
        std::byte{0x89}, std::byte{0x50}, std::byte{0x4E}, std::byte{0x47},
        std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A}};
    constexpr std::array bmp{std::byte{'B'}, std::byte{'M'}};

    if (begins_with(bytes, jpeg)) return ImageFormat::jpeg;
    if (begins_with(bytes, png)) return ImageFormat::png;
    if (begins_with(bytes, bmp)) return ImageFormat::bmp;
    return ImageFormat::unknown;
}

}  // namespace viewer::core
```

Add `src/core/format_probe.cpp` to `viewer_core` in the top-level `CMakeLists.txt`:

```cmake
target_sources(viewer_core PRIVATE
    src/core/format_probe.cpp
)
```

- [ ] **Step 4: Run format tests**

Run:

```powershell
cmake --build --preset windows-msvc-debug
ctest --test-dir build/windows-msvc-debug -C Debug -R FormatProbe --output-on-failure
```

Expected: four `FormatProbe` tests pass.

- [ ] **Step 5: Commit**

```powershell
git add include/viewer/core/format_probe.h src/core/format_probe.cpp tests CMakeLists.txt
git commit -m "feat: detect core image formats by signature"
```

### Task 4: Implement natural filename ordering

**Files:**
- Create: `include/viewer/core/natural_sort.h`
- Create: `src/core/natural_sort.cpp`
- Create: `tests/core/natural_sort_test.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing ordering tests**

Create `tests/core/natural_sort_test.cpp`:

```cpp
#include <algorithm>
#include <string>
#include <vector>
#include <gtest/gtest.h>

#include "viewer/core/natural_sort.h"

using viewer::core::NaturalLess;

TEST(NaturalSort, OrdersNumericRunsByValue) {
    std::vector<std::wstring> names{L"10.jpg", L"2.jpg", L"1.jpg"};
    std::sort(names.begin(), names.end(), NaturalLess{});
    EXPECT_EQ(names, (std::vector<std::wstring>{L"1.jpg", L"2.jpg", L"10.jpg"}));
}

TEST(NaturalSort, ComparesCaseInsensitively) {
    EXPECT_FALSE(NaturalLess{}(L"A.jpg", L"a.jpg"));
    EXPECT_FALSE(NaturalLess{}(L"a.jpg", L"A.jpg"));
}

TEST(NaturalSort, UsesLeadingZeroCountAsStableTieBreaker) {
    EXPECT_TRUE(NaturalLess{}(L"2.jpg", L"002.jpg"));
}
```

Add the file to `viewer_tests`.

- [ ] **Step 2: Verify the tests fail to compile**

Run:

```powershell
cmake --build --preset windows-msvc-debug
```

Expected: missing `viewer/core/natural_sort.h`.

- [ ] **Step 3: Implement numeric-run comparison**

Create `include/viewer/core/natural_sort.h`:

```cpp
#pragma once

#include <string_view>

namespace viewer::core {

struct NaturalLess {
    [[nodiscard]] bool operator()(
        std::wstring_view left,
        std::wstring_view right) const noexcept;
};

}  // namespace viewer::core
```

Create `src/core/natural_sort.cpp`:

```cpp
#include "viewer/core/natural_sort.h"

#include <cwctype>

namespace viewer::core {
namespace {

std::size_t skip_zeros(std::wstring_view value, std::size_t pos) {
    while (pos < value.size() && value[pos] == L'0') ++pos;
    return pos;
}

std::size_t digit_end(std::wstring_view value, std::size_t pos) {
    while (pos < value.size() && std::iswdigit(value[pos])) ++pos;
    return pos;
}

}  // namespace

bool NaturalLess::operator()(
    std::wstring_view left,
    std::wstring_view right) const noexcept {
    std::size_t li = 0;
    std::size_t ri = 0;

    while (li < left.size() && ri < right.size()) {
        if (std::iswdigit(left[li]) && std::iswdigit(right[ri])) {
            const auto left_digits_end = digit_end(left, li);
            const auto right_digits_end = digit_end(right, ri);
            const auto left_value_start = skip_zeros(left, li);
            const auto right_value_start = skip_zeros(right, ri);
            const auto left_value_length = left_digits_end - left_value_start;
            const auto right_value_length = right_digits_end - right_value_start;

            if (left_value_length != right_value_length) {
                return left_value_length < right_value_length;
            }
            const auto left_value = left.substr(left_value_start, left_value_length);
            const auto right_value = right.substr(right_value_start, right_value_length);
            if (left_value != right_value) return left_value < right_value;

            const auto left_run_length = left_digits_end - li;
            const auto right_run_length = right_digits_end - ri;
            if (left_run_length != right_run_length) {
                return left_run_length < right_run_length;
            }
            li = left_digits_end;
            ri = right_digits_end;
            continue;
        }

        const auto lc = std::towlower(left[li]);
        const auto rc = std::towlower(right[ri]);
        if (lc != rc) return lc < rc;
        ++li;
        ++ri;
    }
    return left.size() < right.size();
}

}  // namespace viewer::core
```

Add `src/core/natural_sort.cpp` to `viewer_core`.

- [ ] **Step 4: Run ordering tests**

Run:

```powershell
cmake --build --preset windows-msvc-debug
ctest --test-dir build/windows-msvc-debug -C Debug -R NaturalSort --output-on-failure
```

Expected: all `NaturalSort` tests pass.

- [ ] **Step 5: Commit**

```powershell
git add include/viewer/core/natural_sort.h src/core/natural_sort.cpp tests CMakeLists.txt
git commit -m "feat: add natural filename ordering"
```

### Task 5: Build directory navigation around image candidates

**Files:**
- Create: `include/viewer/core/directory_navigator.h`
- Create: `src/core/directory_navigator.cpp`
- Create: `tests/core/directory_navigator_test.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing navigation tests**

Create `tests/core/directory_navigator_test.cpp`:

```cpp
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

#include "viewer/core/directory_navigator.h"

namespace fs = std::filesystem;
using viewer::core::DirectoryNavigator;

class DirectoryNavigatorTest : public testing::Test {
protected:
    void SetUp() override {
        root = fs::temp_directory_path() / L"fast_viewer_navigator_test";
        fs::remove_all(root);
        fs::create_directories(root);
        write(L"10.jpg", "\xFF\xD8\xFF");
        write(L"2.png", "\x89PNG\r\n\x1A\n");
        write(L"notes.txt", "not an image");
    }

    void TearDown() override { fs::remove_all(root); }

    void write(const wchar_t* name, const char* bytes) {
        std::ofstream stream(root / name, std::ios::binary);
        stream.write(bytes, static_cast<std::streamsize>(std::char_traits<char>::length(bytes)));
    }

    fs::path root;
};

TEST_F(DirectoryNavigatorTest, ScansSupportedCandidatesInNaturalOrder) {
    auto result = DirectoryNavigator::scan(root / L"10.jpg");
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value().items().size(), 2u);
    EXPECT_EQ(result.value().items()[0].filename(), L"2.png");
    EXPECT_EQ(result.value().items()[1].filename(), L"10.jpg");
    EXPECT_EQ(result.value().current_index(), 1u);
}

TEST_F(DirectoryNavigatorTest, WrapsPreviousAndNext) {
    auto result = DirectoryNavigator::scan(root / L"2.png");
    ASSERT_TRUE(result.has_value());
    auto navigator = std::move(result.value());
    EXPECT_EQ(navigator.previous().filename(), L"10.jpg");
    EXPECT_EQ(navigator.next().filename(), L"2.png");
}
```

Add this source to `viewer_tests`.

- [ ] **Step 2: Build and verify missing navigator failure**

Run:

```powershell
cmake --build --preset windows-msvc-debug
```

Expected: missing navigator declarations.

- [ ] **Step 3: Implement directory scanning**

Create `include/viewer/core/directory_navigator.h`:

```cpp
#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>

#include "viewer/core/result.h"

namespace viewer::core {

class DirectoryNavigator {
public:
    static Result<DirectoryNavigator> scan(
        const std::filesystem::path& selected_path);

    [[nodiscard]] const std::vector<std::filesystem::path>& items() const noexcept {
        return items_;
    }
    [[nodiscard]] std::size_t current_index() const noexcept { return index_; }
    [[nodiscard]] const std::filesystem::path& current() const { return items_.at(index_); }
    const std::filesystem::path& previous();
    const std::filesystem::path& next();

private:
    std::vector<std::filesystem::path> items_;
    std::size_t index_ = 0;
};

}  // namespace viewer::core
```

Create `src/core/directory_navigator.cpp`:

```cpp
#include "viewer/core/directory_navigator.h"

#include <algorithm>
#include <array>
#include <fstream>

#include "viewer/core/format_probe.h"
#include "viewer/core/natural_sort.h"

namespace viewer::core {
namespace {

ImageFormat probe_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::array<std::byte, 16> header{};
    stream.read(reinterpret_cast<char*>(header.data()),
                static_cast<std::streamsize>(header.size()));
    return probe_format(
        std::span<const std::byte>(header.data(),
                                  static_cast<std::size_t>(stream.gcount())));
}

}  // namespace

Result<DirectoryNavigator> DirectoryNavigator::scan(
    const std::filesystem::path& selected_path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(selected_path, error)) {
        return Result<DirectoryNavigator>::failure(
            {ErrorCode::io_error, L"Selected path is not a readable file."});
    }

    DirectoryNavigator navigator;
    for (const auto& entry :
         std::filesystem::directory_iterator(selected_path.parent_path(), error)) {
        if (error) break;
        if (!entry.is_regular_file(error)) continue;
        if (probe_file(entry.path()) != ImageFormat::unknown) {
            navigator.items_.push_back(entry.path());
        }
    }
    if (error) {
        return Result<DirectoryNavigator>::failure(
            {ErrorCode::io_error, L"Could not scan the containing directory."});
    }

    std::sort(navigator.items_.begin(), navigator.items_.end(),
              [](const auto& left, const auto& right) {
                  return NaturalLess{}(
                      left.filename().wstring(),
                      right.filename().wstring());
              });
    const auto selected = std::find(
        navigator.items_.begin(), navigator.items_.end(), selected_path);
    if (selected == navigator.items_.end()) {
        return Result<DirectoryNavigator>::failure(
            {ErrorCode::unsupported_format, L"Selected file is not supported."});
    }
    navigator.index_ = static_cast<std::size_t>(
        std::distance(navigator.items_.begin(), selected));
    return Result<DirectoryNavigator>::success(std::move(navigator));
}

const std::filesystem::path& DirectoryNavigator::previous() {
    index_ = index_ == 0 ? items_.size() - 1 : index_ - 1;
    return current();
}

const std::filesystem::path& DirectoryNavigator::next() {
    index_ = (index_ + 1) % items_.size();
    return current();
}

}  // namespace viewer::core
```

Add `src/core/directory_navigator.cpp` to `viewer_core`.

- [ ] **Step 4: Run navigation tests**

Run:

```powershell
cmake --build --preset windows-msvc-debug
ctest --test-dir build/windows-msvc-debug -C Debug -R DirectoryNavigator --output-on-failure
```

Expected: both navigation tests pass.

- [ ] **Step 5: Commit**

```powershell
git add include/viewer/core/directory_navigator.h src/core/directory_navigator.cpp tests CMakeLists.txt
git commit -m "feat: scan and navigate image directories"
```

### Task 6: Implement a cancellable priority executor

**Files:**
- Create: `include/viewer/core/priority_executor.h`
- Create: `src/core/priority_executor.cpp`
- Create: `tests/core/priority_executor_test.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing priority and shutdown tests**

Create `tests/core/priority_executor_test.cpp`:

```cpp
#include <condition_variable>
#include <mutex>
#include <vector>
#include <gtest/gtest.h>

#include "viewer/core/priority_executor.h"

using viewer::core::Priority;
using viewer::core::PriorityExecutor;

TEST(PriorityExecutor, RunsHigherPriorityQueuedWorkFirst) {
    PriorityExecutor executor(1, false);
    std::vector<int> order;
    std::mutex mutex;

    executor.submit(Priority::background, [&] {
        std::scoped_lock lock(mutex);
        order.push_back(3);
    });
    executor.submit(Priority::current_image, [&] {
        std::scoped_lock lock(mutex);
        order.push_back(1);
    });
    executor.submit(Priority::visible_thumbnail, [&] {
        std::scoped_lock lock(mutex);
        order.push_back(2);
    });

    executor.start();
    executor.wait_idle();
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(PriorityExecutor, RejectsNewWorkAfterStop) {
    PriorityExecutor executor(1);
    executor.stop();
    EXPECT_FALSE(executor.submit(Priority::background, [] {}));
}
```

Add this source to `viewer_tests`.

- [ ] **Step 2: Build and verify missing executor failure**

Run:

```powershell
cmake --build --preset windows-msvc-debug
```

Expected: missing `priority_executor.h`.

- [ ] **Step 3: Implement the queue and worker lifecycle**

Create `include/viewer/core/priority_executor.h`:

```cpp
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace viewer::core {

enum class Priority : std::uint8_t {
    current_image = 0,
    visible_thumbnail = 1,
    adjacent_image = 2,
    near_thumbnail = 3,
    background = 4
};

class PriorityExecutor {
public:
    explicit PriorityExecutor(std::size_t thread_count, bool start_immediately = true);
    ~PriorityExecutor();

    PriorityExecutor(const PriorityExecutor&) = delete;
    PriorityExecutor& operator=(const PriorityExecutor&) = delete;

    bool submit(Priority priority, std::function<void()> work);
    void start();
    void wait_idle();
    void stop();

private:
    struct Task {
        Priority priority;
        std::uint64_t sequence;
        std::function<void()> work;
    };
    struct Later {
        bool operator()(const Task& left, const Task& right) const {
            if (left.priority != right.priority) {
                return left.priority > right.priority;
            }
            return left.sequence > right.sequence;
        }
    };

    void worker_loop();

    std::size_t thread_count_;
    std::mutex mutex_;
    std::condition_variable work_ready_;
    std::condition_variable idle_;
    std::priority_queue<Task, std::vector<Task>, Later> queue_;
    std::vector<std::thread> workers_;
    std::uint64_t next_sequence_ = 0;
    std::size_t active_ = 0;
    bool started_ = false;
    bool stopping_ = false;
};

}  // namespace viewer::core
```

Create `src/core/priority_executor.cpp`:

```cpp
#include "viewer/core/priority_executor.h"

namespace viewer::core {

PriorityExecutor::PriorityExecutor(
    std::size_t thread_count,
    bool start_immediately)
    : thread_count_(thread_count == 0 ? 1 : thread_count) {
    if (start_immediately) start();
}

PriorityExecutor::~PriorityExecutor() {
    stop();
}

bool PriorityExecutor::submit(Priority priority, std::function<void()> work) {
    {
        std::scoped_lock lock(mutex_);
        if (stopping_) return false;
        queue_.push(Task{priority, next_sequence_++, std::move(work)});
    }
    work_ready_.notify_one();
    return true;
}

void PriorityExecutor::start() {
    std::scoped_lock lock(mutex_);
    if (started_ || stopping_) return;
    started_ = true;
    for (std::size_t i = 0; i < thread_count_; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

void PriorityExecutor::wait_idle() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return queue_.empty() && active_ == 0; });
}

void PriorityExecutor::stop() {
    {
        std::scoped_lock lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    work_ready_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();
}

void PriorityExecutor::worker_loop() {
    for (;;) {
        Task task;
        {
            std::unique_lock lock(mutex_);
            work_ready_.wait(lock, [this] {
                return stopping_ || !queue_.empty();
            });
            if (stopping_ && queue_.empty()) return;
            task = queue_.top();
            queue_.pop();
            ++active_;
        }
        task.work();
        {
            std::scoped_lock lock(mutex_);
            --active_;
            if (queue_.empty() && active_ == 0) idle_.notify_all();
        }
    }
}

}  // namespace viewer::core
```

Add `src/core/priority_executor.cpp` to `viewer_core`.

- [ ] **Step 4: Run executor tests repeatedly**

Run:

```powershell
cmake --build --preset windows-msvc-debug
1..20 | ForEach-Object {
  ctest --test-dir build/windows-msvc-debug -C Debug -R PriorityExecutor --output-on-failure
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
```

Expected: both tests pass in all 20 runs.

- [ ] **Step 5: Commit**

```powershell
git add include/viewer/core/priority_executor.h src/core/priority_executor.cpp tests CMakeLists.txt
git commit -m "feat: add priority background executor"
```

### Task 7: Decode JPEG, PNG, and BMP through WIC

**Files:**
- Create: `include/viewer/platform/wic_decoder.h`
- Create: `src/platform/wic_decoder.cpp`
- Create: `tools/make_test_fixtures.ps1`
- Create: `tests/platform/wic_decoder_test.cpp`
- Create by script: `tests/fixtures/1x1.bmp`
- Create by script: `tests/fixtures/1x1.png`
- Create by script: `tests/fixtures/corrupt.png`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Add deterministic fixture generation**

Create `tools/make_test_fixtures.ps1`:

```powershell
$ErrorActionPreference = "Stop"
$fixtureDir = Join-Path $PSScriptRoot "..\tests\fixtures"
New-Item -ItemType Directory -Force -Path $fixtureDir | Out-Null

$bmp = [byte[]](
  0x42,0x4D,0x3A,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x36,0x00,0x00,0x00,
  0x28,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x00,
  0x18,0x00,0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x13,0x0B,0x00,0x00,
  0x13,0x0B,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0xFF,0x00
)
[IO.File]::WriteAllBytes((Join-Path $fixtureDir "1x1.bmp"), $bmp)

$pngBase64 = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M/wHwAF/gL+3MxZ5wAAAABJRU5ErkJggg=="
[IO.File]::WriteAllBytes(
  (Join-Path $fixtureDir "1x1.png"),
  [Convert]::FromBase64String($pngBase64))

[IO.File]::WriteAllBytes(
  (Join-Path $fixtureDir "corrupt.png"),
  [byte[]](0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00))
```

Run:

```powershell
powershell -ExecutionPolicy Bypass -File tools/make_test_fixtures.ps1
```

Expected: three files appear in `tests/fixtures`.

- [ ] **Step 2: Write failing WIC decoder tests**

Create `tests/platform/wic_decoder_test.cpp`:

```cpp
#include <filesystem>
#include <gtest/gtest.h>

#include "viewer/platform/wic_decoder.h"

using viewer::platform::WicDecoder;

TEST(WicDecoder, DecodesPngToPremultipliedBgra) {
    WicDecoder decoder;
    auto result = decoder.decode(TEST_FIXTURE_DIR L"/1x1.png", 64u * 1024u);
    ASSERT_TRUE(result.has_value()) << "WIC decode failed";
    EXPECT_EQ(result.value().width, 1u);
    EXPECT_EQ(result.value().height, 1u);
    EXPECT_EQ(result.value().stride, 4u);
}

TEST(WicDecoder, DecodesBmp) {
    WicDecoder decoder;
    auto result = decoder.decode(TEST_FIXTURE_DIR L"/1x1.bmp", 64u * 1024u);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().pixels.size(), 4u);
}

TEST(WicDecoder, ReportsCorruptInput) {
    WicDecoder decoder;
    auto result = decoder.decode(TEST_FIXTURE_DIR L"/corrupt.png", 64u * 1024u);
    EXPECT_FALSE(result.has_value());
}
```

Add the source to `viewer_tests`, link `viewer_wic`, and define the fixture directory:

```cmake
target_compile_definitions(viewer_tests PRIVATE
    TEST_FIXTURE_DIR=L"${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
```

- [ ] **Step 3: Build and verify missing WIC target failure**

Run:

```powershell
cmake --build --preset windows-msvc-debug
```

Expected: failure because `viewer_wic` and `WicDecoder` do not exist.

- [ ] **Step 4: Implement COM-safe WIC decoding**

Create `include/viewer/platform/wic_decoder.h`:

```cpp
#pragma once

#include <cstddef>
#include <filesystem>

#include "viewer/core/image_frame.h"
#include "viewer/core/result.h"

namespace viewer::platform {

class WicDecoder {
public:
    core::Result<core::ImageFrame> decode(
        const std::filesystem::path& path,
        std::size_t byte_budget) const;
};

}  // namespace viewer::platform
```

Create `src/platform/wic_decoder.cpp`:

```cpp
#include "viewer/platform/wic_decoder.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace viewer::platform {
namespace {

core::Error platform_error(const wchar_t* message) {
    return {core::ErrorCode::platform_error, message};
}

class ComApartment {
public:
    ComApartment() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(result_)) CoUninitialize();
    }
    [[nodiscard]] bool usable() const {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT result_;
};

}  // namespace

core::Result<core::ImageFrame> WicDecoder::decode(
    const std::filesystem::path& path,
    std::size_t byte_budget) const {
    ComApartment apartment;
    if (!apartment.usable()) {
        return core::Result<core::ImageFrame>::failure(
            platform_error(L"COM initialization failed."));
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory2,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return core::Result<core::ImageFrame>::failure(
            platform_error(L"WIC factory creation failed."));
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(
        path.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &decoder);
    if (FAILED(hr)) {
        return core::Result<core::ImageFrame>::failure(
            {core::ErrorCode::decode_error, L"WIC could not open the image."});
    }

    ComPtr<IWICBitmapFrameDecode> source;
    hr = decoder->GetFrame(0, &source);
    if (FAILED(hr)) {
        return core::Result<core::ImageFrame>::failure(
            {core::ErrorCode::decode_error, L"WIC could not read frame zero."});
    }

    UINT width = 0;
    UINT height = 0;
    hr = source->GetSize(&width, &height);
    if (FAILED(hr)) {
        return core::Result<core::ImageFrame>::failure(
            {core::ErrorCode::decode_error, L"WIC could not read dimensions."});
    }

    auto frame = core::ImageFrame::allocate_bgra8(width, height, byte_budget);
    if (!frame.has_value()) return frame;

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        return core::Result<core::ImageFrame>::failure(
            platform_error(L"WIC converter creation failed."));
    }
    hr = converter->Initialize(
        source.Get(),
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        return core::Result<core::ImageFrame>::failure(
            {core::ErrorCode::decode_error, L"WIC pixel conversion failed."});
    }

    auto& output = frame.value();
    hr = converter->CopyPixels(
        nullptr,
        output.stride,
        static_cast<UINT>(output.pixels.size()),
        reinterpret_cast<BYTE*>(output.pixels.data()));
    if (FAILED(hr)) {
        return core::Result<core::ImageFrame>::failure(
            {core::ErrorCode::decode_error, L"WIC pixel copy failed."});
    }
    return frame;
}

}  // namespace viewer::platform
```

Add the Windows library:

```cmake
add_library(viewer_wic STATIC
    src/platform/wic_decoder.cpp
)
target_include_directories(viewer_wic PUBLIC include)
target_compile_features(viewer_wic PUBLIC cxx_std_20)
target_link_libraries(viewer_wic PUBLIC viewer_core windowscodecs ole32)
viewer_enable_warnings(viewer_wic)
```

- [ ] **Step 5: Run WIC tests**

Run:

```powershell
cmake --build --preset windows-msvc-debug
ctest --test-dir build/windows-msvc-debug -C Debug -R WicDecoder --output-on-failure
```

Expected: PNG and BMP tests pass; corrupt PNG returns an error without crashing.

- [ ] **Step 6: Commit**

```powershell
git add include/viewer/platform src/platform tools tests CMakeLists.txt
git commit -m "feat: decode baseline formats with WIC"
```

### Task 8: Implement deterministic view transforms

**Files:**
- Create: `include/viewer/core/view_transform.h`
- Create: `src/core/view_transform.cpp`
- Create: `tests/core/view_transform_test.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing fit, zoom, pan, and rotation tests**

Create `tests/core/view_transform_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include "viewer/core/view_transform.h"

using viewer::core::Rotation;
using viewer::core::ViewTransform;

TEST(ViewTransform, FitsImageInsideViewport) {
    ViewTransform transform;
    transform.fit({4000, 2000}, {1000, 1000});
    EXPECT_FLOAT_EQ(transform.scale(), 0.25f);
}

TEST(ViewTransform, OneToOneSetsUnitScale) {
    ViewTransform transform;
    transform.one_to_one();
    EXPECT_FLOAT_EQ(transform.scale(), 1.0f);
}

TEST(ViewTransform, ZoomIsClamped) {
    ViewTransform transform;
    transform.zoom_by(1000.0f);
    EXPECT_FLOAT_EQ(transform.scale(), 64.0f);
    transform.zoom_by(0.00001f);
    EXPECT_FLOAT_EQ(transform.scale(), 0.01f);
}

TEST(ViewTransform, RotationCyclesClockwise) {
    ViewTransform transform;
    transform.rotate_clockwise();
    EXPECT_EQ(transform.rotation(), Rotation::degrees_90);
    transform.rotate_clockwise();
    transform.rotate_clockwise();
    transform.rotate_clockwise();
    EXPECT_EQ(transform.rotation(), Rotation::degrees_0);
}
```

Add this source to `viewer_tests`.

- [ ] **Step 2: Build and verify missing transform failure**

Run:

```powershell
cmake --build --preset windows-msvc-debug
```

Expected: missing `view_transform.h`.

- [ ] **Step 3: Implement the transform state**

Create `include/viewer/core/view_transform.h`:

```cpp
#pragma once

#include <cstdint>

namespace viewer::core {

struct SizeU {
    std::uint32_t width;
    std::uint32_t height;
};

enum class Rotation {
    degrees_0,
    degrees_90,
    degrees_180,
    degrees_270
};

class ViewTransform {
public:
    void fit(SizeU image, SizeU viewport);
    void one_to_one();
    void zoom_by(float factor);
    void pan_by(float dx, float dy);
    void rotate_clockwise();

    [[nodiscard]] float scale() const noexcept { return scale_; }
    [[nodiscard]] float offset_x() const noexcept { return offset_x_; }
    [[nodiscard]] float offset_y() const noexcept { return offset_y_; }
    [[nodiscard]] Rotation rotation() const noexcept { return rotation_; }

private:
    float scale_ = 1.0f;
    float offset_x_ = 0.0f;
    float offset_y_ = 0.0f;
    Rotation rotation_ = Rotation::degrees_0;
};

}  // namespace viewer::core
```

Create `src/core/view_transform.cpp`:

```cpp
#include "viewer/core/view_transform.h"

#include <algorithm>

namespace viewer::core {

void ViewTransform::fit(SizeU image, SizeU viewport) {
    if (image.width == 0 || image.height == 0 ||
        viewport.width == 0 || viewport.height == 0) {
        scale_ = 1.0f;
        return;
    }
    const auto x = static_cast<float>(viewport.width) / image.width;
    const auto y = static_cast<float>(viewport.height) / image.height;
    scale_ = std::min(x, y);
    offset_x_ = 0.0f;
    offset_y_ = 0.0f;
}

void ViewTransform::one_to_one() {
    scale_ = 1.0f;
    offset_x_ = 0.0f;
    offset_y_ = 0.0f;
}

void ViewTransform::zoom_by(float factor) {
    scale_ = std::clamp(scale_ * factor, 0.01f, 64.0f);
}

void ViewTransform::pan_by(float dx, float dy) {
    offset_x_ += dx;
    offset_y_ += dy;
}

void ViewTransform::rotate_clockwise() {
    rotation_ = static_cast<Rotation>(
        (static_cast<int>(rotation_) + 1) % 4);
}

}  // namespace viewer::core
```

Add `src/core/view_transform.cpp` to `viewer_core`.

- [ ] **Step 4: Run transform tests**

Run:

```powershell
cmake --build --preset windows-msvc-debug
ctest --test-dir build/windows-msvc-debug -C Debug -R ViewTransform --output-on-failure
```

Expected: four transform tests pass.

- [ ] **Step 5: Commit**

```powershell
git add include/viewer/core/view_transform.h src/core/view_transform.cpp tests CMakeLists.txt
git commit -m "feat: add image view transforms"
```

### Task 9: Create a Direct3D 11 and Direct2D renderer

**Files:**
- Create: `include/viewer/render/d3d_renderer.h`
- Create: `src/render/d3d_renderer.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add a renderer interface that cannot yet link**

Create `include/viewer/render/d3d_renderer.h`:

```cpp
#pragma once

#include <Windows.h>
#include <memory>

#include "viewer/core/image_frame.h"
#include "viewer/core/result.h"
#include "viewer/core/view_transform.h"

namespace viewer::render {

class D3dRenderer {
public:
    D3dRenderer();
    ~D3dRenderer();
    D3dRenderer(D3dRenderer&&) noexcept;
    D3dRenderer& operator=(D3dRenderer&&) noexcept;
    D3dRenderer(const D3dRenderer&) = delete;
    D3dRenderer& operator=(const D3dRenderer&) = delete;

    core::Result<bool> initialize(HWND window);
    core::Result<bool> resize(unsigned width, unsigned height);
    core::Result<bool> set_image(const core::ImageFrame& frame);
    core::Result<bool> draw(const core::ViewTransform& transform);
    void clear_image();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace viewer::render
```

Temporarily add a compile-only source reference in `CMakeLists.txt`:

```cmake
add_library(viewer_render STATIC
    src/render/d3d_renderer.cpp
)
```

- [ ] **Step 2: Build and verify the missing source failure**

Run:

```powershell
cmake --build --preset windows-msvc-debug
```

Expected: CMake or compiler failure because `src/render/d3d_renderer.cpp` is absent.

- [ ] **Step 3: Implement device, swap chain, bitmap upload, and drawing**

Create `src/render/d3d_renderer.cpp` with these required operations:

```cpp
#include "viewer/render/d3d_renderer.h"

#include <d2d1_1.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace viewer::render {

struct D3dRenderer::Impl {
    HWND window = nullptr;
    ComPtr<ID3D11Device> d3d_device;
    ComPtr<ID3D11DeviceContext> d3d_context;
    ComPtr<IDXGISwapChain1> swap_chain;
    ComPtr<ID2D1Factory1> d2d_factory;
    ComPtr<ID2D1Device> d2d_device;
    ComPtr<ID2D1DeviceContext> d2d_context;
    ComPtr<ID2D1Bitmap1> target;
    ComPtr<ID2D1Bitmap1> image;

    HRESULT create_target() {
        ComPtr<IDXGISurface> surface;
        HRESULT hr = swap_chain->GetBuffer(0, IID_PPV_ARGS(&surface));
        if (FAILED(hr)) return hr;
        const D2D1_BITMAP_PROPERTIES1 properties{
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f,
            96.0f};
        hr = d2d_context->CreateBitmapFromDxgiSurface(
            surface.Get(), &properties, &target);
        if (SUCCEEDED(hr)) d2d_context->SetTarget(target.Get());
        return hr;
    }
};

namespace {

core::Result<bool> failed(const wchar_t* message) {
    return core::Result<bool>::failure(
        {core::ErrorCode::platform_error, message});
}

}  // namespace

D3dRenderer::D3dRenderer() : impl_(std::make_unique<Impl>()) {}
D3dRenderer::~D3dRenderer() = default;
D3dRenderer::D3dRenderer(D3dRenderer&&) noexcept = default;
D3dRenderer& D3dRenderer::operator=(D3dRenderer&&) noexcept = default;

core::Result<bool> D3dRenderer::initialize(HWND window) {
    impl_->window = window;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
        D3D11_SDK_VERSION, &impl_->d3d_device, &level, &impl_->d3d_context);
    if (FAILED(hr)) return failed(L"Direct3D device creation failed.");

    ComPtr<IDXGIDevice> dxgi_device;
    hr = impl_->d3d_device.As(&dxgi_device);
    if (FAILED(hr)) return failed(L"DXGI device query failed.");

    D2D1_FACTORY_OPTIONS options{};
#if defined(_DEBUG)
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        options,
        &impl_->d2d_factory);
    if (FAILED(hr)) return failed(L"Direct2D factory creation failed.");
    hr = impl_->d2d_factory->CreateDevice(dxgi_device.Get(), &impl_->d2d_device);
    if (FAILED(hr)) return failed(L"Direct2D device creation failed.");
    hr = impl_->d2d_device->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &impl_->d2d_context);
    if (FAILED(hr)) return failed(L"Direct2D context creation failed.");

    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    dxgi_device->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.Scaling = DXGI_SCALING_STRETCH;
    hr = factory->CreateSwapChainForHwnd(
        impl_->d3d_device.Get(), window, &description, nullptr, nullptr,
        &impl_->swap_chain);
    if (FAILED(hr)) return failed(L"Swap chain creation failed.");
    hr = impl_->create_target();
    if (FAILED(hr)) return failed(L"Render target creation failed.");
    return core::Result<bool>::success(true);
}

core::Result<bool> D3dRenderer::resize(unsigned width, unsigned height) {
    if (!impl_->swap_chain || width == 0 || height == 0) {
        return core::Result<bool>::success(true);
    }
    impl_->d2d_context->SetTarget(nullptr);
    impl_->target.Reset();
    HRESULT hr = impl_->swap_chain->ResizeBuffers(
        0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) return failed(L"Swap chain resize failed.");
    hr = impl_->create_target();
    if (FAILED(hr)) return failed(L"Render target recreation failed.");
    return core::Result<bool>::success(true);
}

core::Result<bool> D3dRenderer::set_image(const core::ImageFrame& frame) {
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM,
            D2D1_ALPHA_MODE_PREMULTIPLIED));
    HRESULT hr = impl_->d2d_context->CreateBitmap(
        D2D1::SizeU(frame.width, frame.height),
        frame.pixels.data(),
        frame.stride,
        properties,
        &impl_->image);
    if (FAILED(hr)) return failed(L"Image texture upload failed.");
    return core::Result<bool>::success(true);
}

core::Result<bool> D3dRenderer::draw(const core::ViewTransform& transform) {
    impl_->d2d_context->BeginDraw();
    impl_->d2d_context->Clear(D2D1::ColorF(0x15171A));
    if (impl_->image) {
        const auto size = impl_->image->GetSize();
        const D2D1_POINT_2F center{size.width / 2.0f, size.height / 2.0f};
        float angle = 0.0f;
        switch (transform.rotation()) {
            case core::Rotation::degrees_90: angle = 90.0f; break;
            case core::Rotation::degrees_180: angle = 180.0f; break;
            case core::Rotation::degrees_270: angle = 270.0f; break;
            default: break;
        }
        const auto matrix =
            D2D1::Matrix3x2F::Scale(transform.scale(), transform.scale(), center) *
            D2D1::Matrix3x2F::Rotation(angle, center) *
            D2D1::Matrix3x2F::Translation(
                transform.offset_x(), transform.offset_y());
        impl_->d2d_context->SetTransform(matrix);
        impl_->d2d_context->DrawBitmap(impl_->image.Get());
        impl_->d2d_context->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    const HRESULT draw_result = impl_->d2d_context->EndDraw();
    if (FAILED(draw_result) && draw_result != D2DERR_RECREATE_TARGET) {
        return failed(L"Direct2D drawing failed.");
    }
    const HRESULT present_result = impl_->swap_chain->Present(1, 0);
    if (FAILED(present_result)) return failed(L"Swap chain presentation failed.");
    return core::Result<bool>::success(true);
}

void D3dRenderer::clear_image() {
    impl_->image.Reset();
}

}  // namespace viewer::render
```

Complete the target:

```cmake
target_include_directories(viewer_render PUBLIC include)
target_compile_features(viewer_render PUBLIC cxx_std_20)
target_link_libraries(viewer_render PUBLIC
    viewer_core
    d2d1
    d3d11
    dxgi
)
viewer_enable_warnings(viewer_render)
```

- [ ] **Step 4: Build the renderer**

Run:

```powershell
cmake --build --preset windows-msvc-debug
```

Expected: `viewer_render.lib` builds with no warnings promoted from project code.

- [ ] **Step 5: Commit**

```powershell
git add include/viewer/render src/render CMakeLists.txt
git commit -m "feat: add Direct3D image renderer"
```

### Task 10: Create the Win32 application shell

**Files:**
- Create: `src/app/resource.h`
- Create: `src/app/app.h`
- Create: `src/app/app.cpp`
- Create: `src/app/main_window.h`
- Create: `src/app/main_window.cpp`
- Create: `src/app/winmain.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add the executable target before its sources exist**

Append to `CMakeLists.txt`:

```cmake
add_executable(fast_viewer WIN32
    src/app/app.cpp
    src/app/main_window.cpp
    src/app/winmain.cpp
)
target_include_directories(fast_viewer PRIVATE include src)
target_compile_features(fast_viewer PRIVATE cxx_std_20)
target_link_libraries(fast_viewer PRIVATE
    viewer_core
    viewer_wic
    viewer_render
    comctl32
    shell32
)
viewer_enable_warnings(fast_viewer)
```

- [ ] **Step 2: Build and verify missing application source failure**

Run:

```powershell
cmake --build --preset windows-msvc-debug
```

Expected: failure listing absent files under `src/app`.

- [ ] **Step 3: Implement process setup and message loop**

Create `src/app/app.h`:

```cpp
#pragma once

#include <Windows.h>

namespace viewer::app {

int run(HINSTANCE instance, int show_command);

}  // namespace viewer::app
```

Create `src/app/winmain.cpp`:

```cpp
#include <Windows.h>

#include "app/app.h"

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int show_command) {
    return viewer::app::run(instance, show_command);
}
```

Create `src/app/main_window.h`:

```cpp
#pragma once

#include <Windows.h>

#include <filesystem>
#include <memory>

namespace viewer::app {

class MainWindow {
public:
    MainWindow();
    ~MainWindow();
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
```

Create `src/app/app.cpp`:

```cpp
#include "app/app.h"

#include <shellapi.h>

#include "app/main_window.h"

namespace viewer::app {

int run(HINSTANCE instance, int show_command) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    MainWindow window;
    if (!window.create(instance, show_command)) return 1;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) window.open_path(argv[1]);
    if (argv) LocalFree(argv);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

}  // namespace viewer::app
```

Create `src/app/main_window.cpp` with a minimal renderer-backed window:

```cpp
#include "app/main_window.h"

#include <memory>

#include "viewer/render/d3d_renderer.h"

namespace viewer::app {

struct MainWindow::Impl {
    HWND window = nullptr;
    render::D3dRenderer renderer;
};

MainWindow::MainWindow() : impl_(std::make_unique<Impl>()) {}
MainWindow::~MainWindow() = default;

bool MainWindow::create(HINSTANCE instance, int show_command) {
    const wchar_t* class_name = L"FastImageViewer.MainWindow";
    WNDCLASSEXW window_class{
        .cbSize = sizeof(WNDCLASSEXW),
        .style = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc = &MainWindow::window_proc,
        .hInstance = instance,
        .hCursor = LoadCursorW(nullptr, IDC_ARROW),
        .lpszClassName = class_name};
    if (!RegisterClassExW(&window_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    impl_->window = CreateWindowExW(
        0, class_name, L"Fast Image Viewer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800,
        nullptr, nullptr, instance, this);
    if (!impl_->window) return false;
    if (!impl_->renderer.initialize(impl_->window).has_value()) return false;
    ShowWindow(impl_->window, show_command);
    UpdateWindow(impl_->window);
    return true;
}

void MainWindow::open_path(const std::filesystem::path&) {}

LRESULT CALLBACK MainWindow::window_proc(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    MainWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(
            window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    return self
        ? self->handle_message(message, wparam, lparam)
        : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT MainWindow::handle_message(
    UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_SIZE:
            impl_->renderer.resize(LOWORD(lparam), HIWORD(lparam));
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(impl_->window, &paint);
            impl_->renderer.draw(core::ViewTransform{});
            EndPaint(impl_->window, &paint);
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
```

Create `src/app/resource.h`:

```cpp
#pragma once

#define ID_APP_ICON 101
```

- [ ] **Step 4: Build and launch the empty viewer**

Run:

```powershell
cmake --build --preset windows-msvc-debug
& .\build\windows-msvc-debug\Debug\fast_viewer.exe
```

Expected: a resizable dark window appears, remains responsive, and closes normally.

- [ ] **Step 5: Commit**

```powershell
git add src/app CMakeLists.txt
git commit -m "feat: add native Windows application shell"
```

### Task 11: Wire asynchronous loading and stale-result suppression

**Files:**
- Modify: `src/app/main_window.cpp`
- Modify: `src/app/main_window.h`
- Create: `tests/core/load_generation_test.cpp`
- Create: `include/viewer/core/load_generation.h`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write a failing generation acceptance test**

Create `tests/core/load_generation_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include "viewer/core/load_generation.h"

using viewer::core::LoadGeneration;

TEST(LoadGeneration, OnlyAcceptsLatestGeneration) {
    LoadGeneration generation;
    const auto first = generation.begin();
    const auto second = generation.begin();
    EXPECT_FALSE(generation.is_current(first));
    EXPECT_TRUE(generation.is_current(second));
}
```

Add this source to `viewer_tests`.

- [ ] **Step 2: Build and verify the missing generation type**

Run:

```powershell
cmake --build --preset windows-msvc-debug
```

Expected: compile failure because `load_generation.h` is absent.

- [ ] **Step 3: Implement generation tracking**

Create `include/viewer/core/load_generation.h`:

```cpp
#pragma once

#include <atomic>
#include <cstdint>

namespace viewer::core {

class LoadGeneration {
public:
    std::uint64_t begin() noexcept {
        return current_.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    [[nodiscard]] bool is_current(std::uint64_t value) const noexcept {
        return current_.load(std::memory_order_relaxed) == value;
    }

private:
    std::atomic_uint64_t current_{0};
};

}  // namespace viewer::core
```

- [ ] **Step 4: Add worker-owned decoding and UI completion messages**

Extend `MainWindow::Impl` in `src/app/main_window.cpp`:

```cpp
static constexpr UINT wm_image_ready = WM_APP + 1;

struct LoadedImage {
    std::uint64_t generation;
    core::Result<core::ImageFrame> result;
};

struct MainWindow::Impl {
    HWND window = nullptr;
    render::D3dRenderer renderer;
    core::PriorityExecutor executor{
        std::max(1u, std::thread::hardware_concurrency() > 1
                         ? std::thread::hardware_concurrency() - 1
                         : 1u)};
    platform::WicDecoder decoder;
    core::LoadGeneration generation;
    core::CancellationSource cancellation;
    core::ViewTransform transform;
};
```

Add required includes:

```cpp
#include <algorithm>
#include <thread>

#include "viewer/core/load_generation.h"
#include "viewer/core/priority_executor.h"
#include "viewer/platform/wic_decoder.h"
```

Replace `open_path`:

```cpp
void MainWindow::open_path(const std::filesystem::path& path) {
    impl_->cancellation.cancel();
    impl_->cancellation = core::CancellationSource{};
    const auto token = impl_->cancellation.token();
    const auto generation = impl_->generation.begin();
    const HWND target = impl_->window;

    impl_->executor.submit(core::Priority::current_image,
        [this, path, token, generation, target] {
            if (token.is_cancelled()) return;
    auto decoded = impl_->decoder.decode(
        path, 512u * 1024u * 1024u);
            if (token.is_cancelled()) return;
            auto payload = std::make_unique<LoadedImage>(
                LoadedImage{generation, std::move(decoded)});
            if (!PostMessageW(
                    target,
                    wm_image_ready,
                    0,
                    reinterpret_cast<LPARAM>(payload.get()))) {
                return;
            }
            payload.release();
        });
}
```

Handle completion before `WM_DESTROY`:

```cpp
case wm_image_ready: {
    std::unique_ptr<LoadedImage> loaded(
        reinterpret_cast<LoadedImage*>(lparam));
    if (!impl_->generation.is_current(loaded->generation)) return 0;
    if (loaded->result.has_value()) {
        impl_->renderer.set_image(loaded->result.value());
        RECT client{};
        GetClientRect(impl_->window, &client);
        impl_->transform.fit(
            {loaded->result.value().width, loaded->result.value().height},
            {static_cast<unsigned>(client.right),
             static_cast<unsigned>(client.bottom)});
    } else {
        impl_->renderer.clear_image();
    }
    InvalidateRect(impl_->window, nullptr, FALSE);
    return 0;
}
```

In `WM_PAINT`, draw `impl_->transform` instead of a temporary transform.

Declare `~MainWindow()` out of line and make shutdown order explicit so no
worker can access destroyed decoder state:

```cpp
MainWindow::~MainWindow() {
    if (impl_) {
        impl_->cancellation.cancel();
        impl_->executor.stop();
    }
}
```

In `WM_DESTROY`, cancel and stop before posting quit:

```cpp
case WM_DESTROY:
    impl_->cancellation.cancel();
    impl_->executor.stop();
    PostQuitMessage(0);
    return 0;
```

- [ ] **Step 5: Run unit tests and manually open two files quickly**

Run:

```powershell
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug --output-on-failure
& .\build\windows-msvc-debug\Debug\fast_viewer.exe C:\path\to\large-a.jpg
```

While the first decode is active, invoke a second process or add temporary debugger calls to `open_path` for `large-b.jpg`.

Expected: all tests pass; only the newest request can replace the displayed frame; closing the window during decode does not hang.

- [ ] **Step 6: Commit**

```powershell
git add include/viewer/core/load_generation.h src/app tests
git commit -m "feat: load images asynchronously"
```

### Task 12: Add directory navigation and adjacent prefetch

**Files:**
- Modify: `src/app/main_window.cpp`
- Modify: `src/app/main_window.h`

- [ ] **Step 1: Introduce navigator state with a deliberately failing build**

Add to `MainWindow::Impl`:

```cpp
std::optional<core::DirectoryNavigator> navigator;
```

Expected: build fails until `<optional>` and the navigator header are included and navigation flow is implemented.

- [ ] **Step 2: Build and capture the failure**

Run:

```powershell
cmake --build --preset windows-msvc-debug
```

Expected: errors for incomplete or missing `DirectoryNavigator`.

- [ ] **Step 3: Scan on open and bind keyboard navigation**

Add includes:

```cpp
#include <optional>
#include "viewer/core/directory_navigator.h"
```

Split loading into a private helper declared in `main_window.h`:

```cpp
void request_image(const std::filesystem::path& path, core::Priority priority);
void navigate(int delta);
```

Change `open_path`:

```cpp
void MainWindow::open_path(const std::filesystem::path& path) {
    auto scan = core::DirectoryNavigator::scan(path);
    if (scan.has_value()) {
        impl_->navigator = std::move(scan.value());
        request_image(impl_->navigator->current(), core::Priority::current_image);
    }
}
```

Move Task 11’s decode submission into `request_image`, using the supplied priority.

Implement navigation:

```cpp
void MainWindow::navigate(int delta) {
    if (!impl_->navigator) return;
    const auto& path =
        delta < 0 ? impl_->navigator->previous() : impl_->navigator->next();
    request_image(path, core::Priority::current_image);
}
```

Handle input:

```cpp
case WM_KEYDOWN:
    if (wparam == VK_LEFT || wparam == VK_UP || wparam == VK_PRIOR) {
        navigate(-1);
        return 0;
    }
    if (wparam == VK_RIGHT || wparam == VK_DOWN || wparam == VK_NEXT ||
        wparam == VK_SPACE) {
        navigate(1);
        return 0;
    }
    break;
```

After current-image completion, submit neighboring items at
`core::Priority::adjacent_image` into a small decoded-frame cache keyed by normalized path. The cache for Phase 1 is exactly three entries: current, previous, next. On navigation, check this cache before decoding and immediately upload a hit.

Use this concrete cache shape in `Impl`:

```cpp
struct CachedFrame {
    std::filesystem::path path;
    std::shared_ptr<core::ImageFrame> frame;
};
std::array<std::optional<CachedFrame>, 3> frame_cache;
```

When inserting, replace the slot whose path is neither current nor immediate neighbor. Do not add a general LRU in Phase 1.

- [ ] **Step 4: Manually verify ordering and responsiveness**

Prepare a directory containing:

```text
1.jpg
2.jpg
10.jpg
notes.txt
```

Run:

```powershell
& .\build\windows-msvc-debug\Debug\fast_viewer.exe C:\path\to\2.jpg
```

Expected:

- Right arrow shows `10.jpg`.
- Left arrow returns to `2.jpg`, then `1.jpg`.
- `notes.txt` never appears.
- Repeated rapid key presses keep the window responsive.
- A prefetched neighbor appears without visible blanking.

- [ ] **Step 5: Run the full test suite**

Run:

```powershell
ctest --preset windows-msvc-debug --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 6: Commit**

```powershell
git add src/app
git commit -m "feat: navigate and prefetch directory images"
```

### Task 13: Add zoom, pan, rotation, fit, and 1:1 input

**Files:**
- Modify: `src/app/main_window.cpp`

- [ ] **Step 1: Add interaction state and make the build fail on unused/incomplete paths**

Add to `MainWindow::Impl`:

```cpp
bool panning = false;
POINT last_pointer{};
```

With `/W4`, leave no unused state: wire it in the next step.

- [ ] **Step 2: Implement input mapping**

Add these message handlers:

```cpp
case WM_MOUSEWHEEL: {
    const auto delta = GET_WHEEL_DELTA_WPARAM(wparam);
    impl_->transform.zoom_by(delta > 0 ? 1.1f : 1.0f / 1.1f);
    InvalidateRect(impl_->window, nullptr, FALSE);
    return 0;
}
case WM_LBUTTONDOWN:
    impl_->panning = true;
    impl_->last_pointer = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    SetCapture(impl_->window);
    return 0;
case WM_MOUSEMOVE:
    if (impl_->panning) {
        const POINT current{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        impl_->transform.pan_by(
            static_cast<float>(current.x - impl_->last_pointer.x),
            static_cast<float>(current.y - impl_->last_pointer.y));
        impl_->last_pointer = current;
        InvalidateRect(impl_->window, nullptr, FALSE);
    }
    return 0;
case WM_LBUTTONUP:
    impl_->panning = false;
    ReleaseCapture();
    return 0;
case WM_KEYDOWN:
    if (wparam == 'F') {
        RECT client{};
        GetClientRect(impl_->window, &client);
        if (auto current = current_cached_frame()) {
            impl_->transform.fit(
                {current->width, current->height},
                {static_cast<unsigned>(client.right),
                 static_cast<unsigned>(client.bottom)});
        }
        InvalidateRect(impl_->window, nullptr, FALSE);
        return 0;
    }
    if (wparam == '1') {
        impl_->transform.one_to_one();
        InvalidateRect(impl_->window, nullptr, FALSE);
        return 0;
    }
    if (wparam == 'R') {
        impl_->transform.rotate_clockwise();
        InvalidateRect(impl_->window, nullptr, FALSE);
        return 0;
    }
    break;
```

Add a private helper returning the currently displayed cached frame:

```cpp
std::shared_ptr<const core::ImageFrame> current_cached_frame() const;
```

Implement it by matching `navigator->current()` against the three cache slots.

- [ ] **Step 3: Build and manually verify interactions**

Run:

```powershell
cmake --build --preset windows-msvc-debug
& .\build\windows-msvc-debug\Debug\fast_viewer.exe C:\path\to\large.jpg
```

Expected:

- Mouse wheel zooms smoothly.
- Left drag pans.
- `F` fits the image.
- `1` sets 1:1.
- `R` rotates clockwise without modifying the file.
- Previous/next remains responsive after transforms.

- [ ] **Step 4: Run core transform tests**

Run:

```powershell
ctest --test-dir build/windows-msvc-debug -C Debug -R ViewTransform --output-on-failure
```

Expected: all transform tests pass.

- [ ] **Step 5: Commit**

```powershell
git add src/app
git commit -m "feat: add core image interactions"
```

### Task 14: Add error presentation and timing instrumentation

**Files:**
- Create: `include/viewer/core/load_metrics.h`
- Create: `src/core/load_metrics.cpp`
- Create: `tests/core/load_metrics_test.cpp`
- Modify: `src/app/main_window.cpp`
- Modify: `src/render/d3d_renderer.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write a failing elapsed-time test**

Create `tests/core/load_metrics_test.cpp`:

```cpp
#include <chrono>
#include <gtest/gtest.h>

#include "viewer/core/load_metrics.h"

using namespace std::chrono_literals;
using viewer::core::LoadMetrics;

TEST(LoadMetrics, RecordsDecodeAndPresentationDurations) {
    LoadMetrics metrics;
    metrics.decode_started = LoadMetrics::Clock::time_point{10ms};
    metrics.decode_finished = LoadMetrics::Clock::time_point{40ms};
    metrics.presented = LoadMetrics::Clock::time_point{55ms};
    EXPECT_EQ(metrics.decode_duration(), 30ms);
    EXPECT_EQ(metrics.total_duration(), 45ms);
}
```

- [ ] **Step 2: Verify missing metrics failure**

Run:

```powershell
cmake --build --preset windows-msvc-debug
```

Expected: missing `load_metrics.h`.

- [ ] **Step 3: Implement metrics**

Create `include/viewer/core/load_metrics.h`:

```cpp
#pragma once

#include <chrono>

namespace viewer::core {

struct LoadMetrics {
    using Clock = std::chrono::steady_clock;
    Clock::time_point requested{};
    Clock::time_point decode_started{};
    Clock::time_point decode_finished{};
    Clock::time_point presented{};

    [[nodiscard]] Clock::duration decode_duration() const {
        return decode_finished - decode_started;
    }
    [[nodiscard]] Clock::duration total_duration() const {
        return presented - requested;
    }
};

}  // namespace viewer::core
```

No `.cpp` is needed; remove `src/core/load_metrics.cpp` from the file list if the header-only implementation is retained.

- [ ] **Step 4: Carry metrics and errors through completion**

Extend `LoadedImage`:

```cpp
struct LoadedImage {
    std::filesystem::path path;
    std::uint64_t generation;
    core::LoadMetrics metrics;
    core::Result<core::ImageFrame> result;
};
```

Set timestamps around decode:

```cpp
core::LoadMetrics metrics;
metrics.requested = core::LoadMetrics::Clock::now();
metrics.decode_started = core::LoadMetrics::Clock::now();
auto decoded = impl_->decoder.decode(path, 512_MiB);
metrics.decode_finished = core::LoadMetrics::Clock::now();
```

On UI completion, set `presented` after the first successful draw and emit one debugger line:

```cpp
wchar_t buffer[256]{};
swprintf_s(
    buffer,
    L"[fast_viewer] decode=%lldus total=%lldus file=%ls\n",
    std::chrono::duration_cast<std::chrono::microseconds>(
        loaded->metrics.decode_duration()).count(),
    std::chrono::duration_cast<std::chrono::microseconds>(
        loaded->metrics.total_duration()).count(),
    loaded->path.c_str());
OutputDebugStringW(buffer);
```

Store the latest error text in `Impl`:

```cpp
std::wstring error_text;
```

On decode failure, set `error_text`; on success, clear it. Extend `D3dRenderer` with:

```cpp
void set_status_text(std::wstring text);
```

Create an `IDWriteFactory`, `IDWriteTextFormat`, and white `ID2D1SolidColorBrush` in renderer initialization. During `draw`, render `status_text` centered in the client area only when no image is present. Use the exact public message:

```text
This image could not be opened.
```

Keep detailed decoder errors in debugger output, not in the default UI.

- [ ] **Step 5: Run tests and error-path smoke test**

Run:

```powershell
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug --output-on-failure
& .\build\windows-msvc-debug\Debug\fast_viewer.exe .\tests\fixtures\corrupt.png
```

Expected:

- All tests pass.
- Corrupt input shows a centered lightweight error.
- The window remains responsive and navigation can continue.
- Debug output contains decode timing and the detailed failure.

- [ ] **Step 6: Commit**

```powershell
git add include/viewer/core/load_metrics.h src/app src/render tests CMakeLists.txt
git commit -m "feat: report load failures and timings"
```

### Task 15: Add Windows CI and phase acceptance checks

**Files:**
- Create: `.github/workflows/windows.yml`
- Create: `docs/performance/phase-1-benchmark.md`
- Modify: `README.md` if it exists; otherwise create it

- [ ] **Step 1: Add a CI workflow**

Create `.github/workflows/windows.yml`:

```yaml
name: windows

on:
  push:
  pull_request:

jobs:
  build-and-test:
    runs-on: windows-2022
    steps:
      - uses: actions/checkout@v4

      - name: Install vcpkg
        uses: lukka/run-vcpkg@v11

      - name: Configure
        run: cmake --preset windows-msvc-debug

      - name: Build
        run: cmake --build --preset windows-msvc-debug

      - name: Test
        run: ctest --preset windows-msvc-debug --output-on-failure
```

- [ ] **Step 2: Add benchmark protocol**

Create `docs/performance/phase-1-benchmark.md`:

```markdown
# Phase 1 benchmark protocol

Record all timings on Windows 10 22H2 or Windows 11 x64 using a 4-core CPU,
8 GB RAM, integrated GPU, and SATA SSD where available.

## Corpus

- JPEG: 24 MP baseline file, approximately 8–15 MB.
- PNG: 3840×2160 opaque file.
- Directory: 1,000 mixed JPEG/PNG/BMP files with numeric names.

## Runs

1. Reboot or sign out before cold-start measurements.
2. Run Release build outside Visual Studio.
3. Measure 10 cold launches and report median plus p95.
4. Measure first image from process start to first successful Present.
5. Traverse 100 images forward, then backward; report cache-hit and cache-miss
   medians separately from debugger metrics.
6. Record peak working set and CPU utilization.

## Phase 1 gates

- Window visible median: ≤ 150 ms on target hardware.
- Baseline JPEG first visible target: ≤ 100 ms.
- Prefetched neighbor target: ≤ 16 ms.
- No UI-thread decode or directory scan in Windows Performance Analyzer.
- No unbounded working-set growth during 1,000-image traversal.
```

- [ ] **Step 3: Add build and usage documentation**

Create `README.md`:

```markdown
# Fast Image Viewer

Windows 10 22H2/Windows 11 x64 native image viewer focused on low-latency
opening and navigation.

## Build

```powershell
$env:VCPKG_ROOT = "C:\src\vcpkg"
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug --output-on-failure
```

## Run

```powershell
.\build\windows-msvc-debug\Debug\fast_viewer.exe C:\Pictures\photo.jpg
```

## Phase 1 controls

- Left/Up/Page Up: previous image
- Right/Down/Page Down/Space: next image
- Mouse wheel: zoom
- Left drag: pan
- F: fit
- 1: 1:1
- R: rotate view clockwise
```

- [ ] **Step 4: Run clean configure, build, and test**

Remove only the generated build directory, then run:

```powershell
Remove-Item -Recurse -Force .\build\windows-msvc-debug
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug --output-on-failure
```

Expected: clean configure and build succeed; every test passes.

- [ ] **Step 5: Run Release smoke test**

Run:

```powershell
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
& .\build\windows-msvc-release\Release\fast_viewer.exe C:\path\to\sample.jpg
```

Expected: Release executable opens the sample, navigates the directory, and closes cleanly.

- [ ] **Step 6: Commit**

```powershell
git add .github README.md docs/performance
git commit -m "ci: verify phase one on Windows"
```

## Phase 1 completion checklist

- [ ] `cmake --preset windows-msvc-debug` succeeds from a clean checkout.
- [ ] `cmake --build --preset windows-msvc-debug` succeeds with `/W4`.
- [ ] `ctest --preset windows-msvc-debug --output-on-failure` passes.
- [ ] Release executable opens JPEG, PNG, and BMP by path.
- [ ] Directory navigation follows natural order and skips unsupported files.
- [ ] Current-image decode runs off the UI thread.
- [ ] Rapid navigation cannot display an obsolete decode result.
- [ ] Previous and next images are prefetched within the fixed three-frame budget.
- [ ] Zoom, pan, fit, 1:1, and view rotation work.
- [ ] Corrupt input shows a lightweight error and does not block navigation.
- [ ] WPA confirms no directory scan or decode work on the UI thread.
- [ ] Benchmark results are recorded in a dated copy of the Phase 1 protocol.

Only after every item passes should Phase 2 thumbnail work begin.
