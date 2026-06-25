# Fast Image Viewer

Windows 10 22H2/Windows 11 x64 native image viewer focused on low-latency
opening, rendering, and previous/next navigation.

Phase 1 supports opening JPEG, PNG, and BMP images through WIC, Direct3D 11 /
Direct2D rendering, natural-order directory navigation, asynchronous decode,
neighbor prefetch, fit-to-window, 1:1, wheel zoom, pan, view rotation, and a
lightweight decode error overlay.

## Requirements

- Windows 10 22H2 or Windows 11 x64.
- Visual Studio 2022 with Desktop development with C++.
- Windows 10/11 SDK.
- CMake 3.28 or newer.
- vcpkg checked out at the manifest baseline commit recorded in `vcpkg.json`.

## Build and test on Windows

From a fresh vcpkg checkout, bootstrap vcpkg first. Then set `VCPKG_ROOT`
before running CMake so the presets can find the vcpkg toolchain file.

```powershell
cd C:\src\vcpkg
.\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "C:\src\vcpkg"
cd C:\src\fast-image-viewer
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug --output-on-failure
```

Release smoke compile:

```powershell
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
```

## Build the platform-neutral core locally

Non-Windows hosts can configure and compile the core library without vcpkg or
Windows SDK dependencies:

```sh
cmake --preset host-core-debug
cmake --build --preset host-core-debug
```

This preset intentionally disables tests because GoogleTest is supplied through
the Windows vcpkg manifest workflow.

## Run

```powershell
.\build\windows-msvc-debug\Debug\fast_viewer.exe C:\Pictures\photo.jpg
```

Open any image in a directory to enable previous/next navigation across the
supported images in that directory.

You can also launch `fast_viewer.exe` without an argument. The empty window
shows a small prompt; press `O` to open an image, or drag an image file into the
window.

## Register file associations

The release artifact includes `register_file_associations.ps1`. Run it from the
same directory as `fast_viewer.exe` to register JPEG, PNG, and BMP files for the
current Windows user:

```powershell
powershell -ExecutionPolicy Bypass -File .\register_file_associations.ps1
```

After registration, supported image files can be opened with FlashView from
Windows Explorer. If Windows already has a default app for an extension, use
"Open with" once to choose FlashView.

## Phase 1 controls

- O: open image
- Left / Up / Page Up: previous image
- Right / Down / Page Down / Space: next image
- Mouse wheel: zoom
- Left drag: pan
- F: fit to window
- 1: 1:1 zoom
- R: rotate view clockwise

## Benchmarking

Use [docs/performance/phase-1-benchmark.md](docs/performance/phase-1-benchmark.md)
for the Phase 1 acceptance protocol and report template.
