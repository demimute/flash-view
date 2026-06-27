# Phase 1 benchmark protocol

Record timings on Windows 10 22H2 or Windows 11 x64. The target baseline is a
4-core CPU, 8 GB RAM, integrated GPU, and SATA SSD when available. Record the
exact CPU, GPU, RAM, storage type, Windows version, power mode, display scale,
build SHA, and whether Windows Defender or another scanner was active.

## Corpus

- JPEG: 24 MP baseline file, approximately 8-15 MB.
- PNG: 3840x2160 opaque file.
- BMP: 3840x2160 24-bit or 32-bit file.
- Directory: 1,000 mixed JPEG/PNG/BMP files with numeric names, including
  names such as `2.jpg` and `10.jpg` to verify natural ordering.
- Corrupt input: one malformed PNG to confirm error handling does not block
  subsequent navigation.

Keep the corpus on local storage. Do not use cloud-synced, network, removable,
or compressed folders for acceptance measurements.

## Build under test

Use a Release build outside Visual Studio:

```powershell
$env:VCPKG_ROOT = "C:\src\vcpkg"
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
```

Run:

```powershell
.\build\windows-msvc-release\Release\FlashView.exe C:\Pictures\phase1\2.jpg
```

## Runs

1. Reboot or sign out before cold-start measurements.
2. Close indexing, backup, and image-editing tools that may touch the corpus.
3. Measure 10 cold launches and report median plus p95 from process start to
   first visible window.
4. Measure first image from process start to first successful present.
5. Traverse 100 images forward, then backward. Report cache-hit and cache-miss
   medians separately using the debug timing output format.
6. Repeat zoom, pan, fit, 1:1, and rotate interactions on the JPEG and PNG
   samples while watching for visible stalls.
7. Record peak working set, CPU utilization, and GPU engine utilization.
8. Capture a Windows Performance Recorder trace for at least one traversal and
   inspect it in Windows Performance Analyzer.

## Phase 1 gates

- Window visible median: <= 150 ms on target hardware.
- Baseline JPEG first visible target: <= 100 ms.
- Prefetched neighbor target: <= 16 ms.
- JPEG, PNG, and BMP open by path in the Release executable.
- Directory navigation follows natural order and skips unsupported files.
- Corrupt input shows a lightweight error and does not block navigation.
- No UI-thread decode or directory scan in Windows Performance Analyzer.
- No unbounded working-set growth during 1,000-image traversal.

## Report template

```text
Date:
Build SHA:
Machine:
Windows version:
Storage:
Display scale:
Corpus path:

Cold window visible median / p95:
First JPEG visible median / p95:
Prefetched neighbor median / p95:
100 forward traversal median / p95:
100 backward traversal median / p95:
Peak working set:
WPA finding:
Notes:
```
