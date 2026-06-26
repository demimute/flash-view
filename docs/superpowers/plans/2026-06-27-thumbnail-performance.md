# Thumbnail Performance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make thumbnail browsing smooth in large or heavy image folders and prevent the thumbnail pane from disappearing while switching images.

**Architecture:** Keep thumbnail UI state independent from current-image loading. The render path must never decode images; it reads thumbnail cache only and schedules background work for visible and nearby items. WIC gets a dedicated thumbnail decode path that uses embedded thumbnails or scaler output instead of full-size decode.

**Tech Stack:** Win32, WIC, existing `PriorityExecutor`, C++20, GTest/source regression tests, GitHub Actions Windows build.

---

### Task 1: Guard current regressions

**Files:**
- Modify: `tests/app/source_regression_test.cpp`

- [ ] Add tests that fail while render code still calls `thumbnail_frame_for(entry.path)` and while `open_path()` clears `thumbnail_entries_cache`.
- [ ] Run local source checks to confirm the tests fail before production edits.

### Task 2: Keep thumbnail pane state stable during image switches

**Files:**
- Modify: `src/app/main_window.cpp`

- [ ] Stop clearing thumbnail browser directory, scroll offset, and entry cache from normal image open.
- [ ] Mark entries dirty only when the directory truly changes.
- [ ] Keep old thumbnail overlay visible while navigator scan for the new image is pending.

### Task 3: Move thumbnail decoding off the UI path

**Files:**
- Modify: `src/app/main_window.cpp`
- Modify: `include/viewer/core/priority_executor.h`

- [ ] Add a thumbnail generation counter and in-flight set.
- [ ] In `make_render_overlay()`, use cache-only lookup and enqueue visible/nearby thumbnails.
- [ ] Add a `thumbnail_ready_message` completion path that stores decoded thumbnail frames and invalidates the window.

### Task 4: Add WIC thumbnail-sized decode

**Files:**
- Modify: `include/viewer/platform/wic_decoder.h`
- Modify: `src/platform/wic_decoder.cpp`
- Modify: `tests/platform/wic_decoder_test.cpp`

- [ ] Add `WicDecoder::decode_thumbnail(path, max_edge, byte_budget)`.
- [ ] Try decoder thumbnail first, then frame thumbnail, then `IWICBitmapScaler`.
- [ ] Ensure GIF thumbnails decode only one small frame.

### Task 5: Verify and package

**Files:**
- Modify as needed based on compiler output.

- [ ] Run local source checks, `cmake --build --preset host-core-debug`, and `git diff --check`.
- [ ] Commit, push, and wait for Windows GitHub Actions.
