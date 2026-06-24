#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string main_window_source() {
  const std::filesystem::path path =
      std::filesystem::path(TEST_SOURCE_DIR) / "src/app/main_window.cpp";
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

TEST(AsyncLoadContractTest, CloseDoesNotSynchronouslyStopExecutor) {
  const std::string source = main_window_source();

  const std::size_t close_case = source.find("case WM_CLOSE:");
  ASSERT_NE(close_case, std::string::npos);
  const std::size_t destroy_case = source.find("case WM_DESTROY:", close_case);
  ASSERT_NE(destroy_case, std::string::npos);
  EXPECT_EQ(source.substr(close_case, destroy_case - close_case).find(".stop()"),
            std::string::npos);
}

TEST(AsyncLoadContractTest, DestroyOnlyPostsQuitMessage) {
  const std::string source = main_window_source();

  const std::size_t destroy_case = source.find("case WM_DESTROY:");
  ASSERT_NE(destroy_case, std::string::npos);
  const std::size_t default_case = source.find("default:", destroy_case);
  ASSERT_NE(default_case, std::string::npos);
  const std::string destroy_block =
      source.substr(destroy_case, default_case - destroy_case);
  EXPECT_NE(destroy_block.find("PostQuitMessage(0)"), std::string::npos);
  EXPECT_EQ(destroy_block.find("shutdown"), std::string::npos);
  EXPECT_EQ(destroy_block.find(".stop()"), std::string::npos);
}

TEST(AsyncLoadContractTest, WorkerNeverCapturesWindowObject) {
  const std::string source = main_window_source();

  EXPECT_EQ(source.find("[this"), std::string::npos);
  EXPECT_EQ(source.find("[impl"), std::string::npos);
}

TEST(AsyncLoadContractTest, UsesNoexceptWindowsThreadpoolReaper) {
  const std::string source = main_window_source();

  EXPECT_NE(source.find("TrySubmitThreadpoolCallback"), std::string::npos);
  EXPECT_NE(source.find("CALLBACK reap_async_load_context"), std::string::npos);
  EXPECT_NE(source.find("noexcept"), std::string::npos);
  EXPECT_EQ(source.find(".detach()"), std::string::npos);
  EXPECT_EQ(source.find("std::thread(["), std::string::npos);
}

TEST(AsyncLoadContractTest, SubmissionFailureIntentionallyRetainsPayload) {
  const std::string source = main_window_source();

  EXPECT_NE(source.find("bounded process-lifetime leak"), std::string::npos);
  EXPECT_NE(source.find("release"), std::string::npos);
}

TEST(AsyncLoadContractTest, MainWindowKeepsDirectoryNavigatorInImpl) {
  const std::string source = main_window_source();

  EXPECT_NE(source.find("std::optional<core::DirectoryNavigator> navigator"),
            std::string::npos);
}

TEST(AsyncLoadContractTest, DirectoryScanRunsOnWorkerNotOpenPath) {
  const std::string source = main_window_source();

  const std::size_t open_path = source.find("void MainWindow::open_path(");
  ASSERT_NE(open_path, std::string::npos);
  const std::size_t window_proc =
      source.find("LRESULT CALLBACK MainWindow::window_proc", open_path);
  ASSERT_NE(window_proc, std::string::npos);
  const std::string open_path_block =
      source.substr(open_path, window_proc - open_path);
  EXPECT_EQ(open_path_block.find("DirectoryNavigator::scan"),
            std::string::npos);
  EXPECT_NE(open_path_block.find("request_image(path, "
                                 "core::Priority::current_image, true)"),
            std::string::npos);
  EXPECT_NE(open_path_block.find("request_directory_scan(path)"),
            std::string::npos);
}

TEST(AsyncLoadContractTest, DirectoryScanPublishesNavigatorReadyMessage) {
  const std::string source = main_window_source();

  const std::size_t request_scan =
      source.find("void request_directory_scan(");
  ASSERT_NE(request_scan, std::string::npos);
  const std::size_t prefetch_neighbors =
      source.find("void prefetch_neighbors()", request_scan);
  ASSERT_NE(prefetch_neighbors, std::string::npos);
  const std::string request_scan_block =
      source.substr(request_scan, prefetch_neighbors - request_scan);

  EXPECT_NE(source.find("constexpr UINT navigator_ready_message"),
            std::string::npos);
  EXPECT_NE(source.find("core::LoadGeneration scan_generation"),
            std::string::npos);
  EXPECT_NE(request_scan_block.find("context->executor.submit"),
            std::string::npos);
  EXPECT_NE(request_scan_block.find("core::DirectoryNavigator::scan(path)"),
            std::string::npos);
  EXPECT_NE(request_scan_block.find("navigator_ready_message"),
            std::string::npos);
  EXPECT_NE(request_scan_block.find("PostMessageW"), std::string::npos);
  EXPECT_EQ(request_scan_block.find("[this"), std::string::npos);
  EXPECT_EQ(request_scan_block.find("[impl"), std::string::npos);
}

TEST(AsyncLoadContractTest, ShutdownDiscardsImageAndNavigatorPayloads) {
  const std::string source = main_window_source();

  const std::size_t discard = source.find("void discard_pending_completions()");
  ASSERT_NE(discard, std::string::npos);
  const std::size_t shutdown = source.find("void shutdown_async()", discard);
  ASSERT_NE(shutdown, std::string::npos);
  const std::string discard_block = source.substr(discard, shutdown - discard);

  EXPECT_NE(discard_block.find("image_ready_message"), std::string::npos);
  EXPECT_NE(discard_block.find("navigator_ready_message"), std::string::npos);
  EXPECT_NE(discard_block.find("LoadedImage"), std::string::npos);
  EXPECT_NE(discard_block.find("LoadedNavigator"), std::string::npos);
}

TEST(AsyncLoadContractTest, MainWindowRequestsDisplayAndPrefetchSeparately) {
  const std::string source = main_window_source();

  EXPECT_NE(source.find("void request_image(const std::filesystem::path& path"),
            std::string::npos);
  EXPECT_NE(source.find("core::Priority::current_image, true"),
            std::string::npos);
  EXPECT_NE(source.find("core::Priority::adjacent_image, false"),
            std::string::npos);
  EXPECT_NE(source.find("display_when_ready ? context->generation.begin()"),
            std::string::npos);
  EXPECT_NE(source.find("LoadedImagePurpose::prefetch"), std::string::npos);
}

TEST(AsyncLoadContractTest, KeyboardInputDelegatesToInputMapping) {
  const std::string source = main_window_source();

  const std::size_t key_case = source.find("case WM_KEYDOWN:");
  ASSERT_NE(key_case, std::string::npos);
  const std::size_t close_case = source.find("case WM_CLOSE:", key_case);
  ASSERT_NE(close_case, std::string::npos);
  const std::string key_block = source.substr(key_case, close_case - key_case);

  EXPECT_NE(key_block.find("classify_key(static_cast<unsigned>(wparam))"),
            std::string::npos);
  EXPECT_EQ(key_block.find("VK_LEFT"), std::string::npos);
  EXPECT_EQ(key_block.find("VK_UP"), std::string::npos);
  EXPECT_EQ(key_block.find("VK_PRIOR"), std::string::npos);
  EXPECT_EQ(key_block.find("VK_RIGHT"), std::string::npos);
  EXPECT_EQ(key_block.find("VK_DOWN"), std::string::npos);
  EXPECT_EQ(key_block.find("VK_NEXT"), std::string::npos);
  EXPECT_EQ(key_block.find("VK_SPACE"), std::string::npos);
}

TEST(AsyncLoadContractTest, UnhandledKeyboardInputFallsThroughToDefWindowProc) {
  const std::string source = main_window_source();

  const std::size_t key_case = source.find("case WM_KEYDOWN:");
  ASSERT_NE(key_case, std::string::npos);
  const std::size_t close_case = source.find("case WM_CLOSE:", key_case);
  ASSERT_NE(close_case, std::string::npos);
  const std::string key_block = source.substr(key_case, close_case - key_case);

  const std::size_t none_case = key_block.find("case KeyAction::none:");
  ASSERT_NE(none_case, std::string::npos);
  const std::string none_block = key_block.substr(none_case);
  EXPECT_NE(key_block.find("break;"), std::string::npos);
  EXPECT_NE(none_block.find("break;"), std::string::npos);
  EXPECT_EQ(none_block.find("return 0;"), std::string::npos);
  EXPECT_NE(source.find("return DefWindowProcW(impl_->window, message, wparam, "
                        "lparam);",
                        close_case),
            std::string::npos);
}

TEST(AsyncLoadContractTest, PointerPanningUsesPanTrackerHelper) {
  const std::string source = main_window_source();

  EXPECT_NE(source.find("PanTracker pan"), std::string::npos);
  EXPECT_EQ(source.find("bool panning = false"), std::string::npos);
  EXPECT_EQ(source.find("POINT last_pointer"), std::string::npos);

  const std::size_t button_down = source.find("case WM_LBUTTONDOWN:");
  ASSERT_NE(button_down, std::string::npos);
  const std::size_t mouse_move = source.find("case WM_MOUSEMOVE:", button_down);
  ASSERT_NE(mouse_move, std::string::npos);
  const std::string down_block =
      source.substr(button_down, mouse_move - button_down);
  EXPECT_NE(down_block.find(".begin("), std::string::npos);
  EXPECT_NE(down_block.find("SetCapture(impl_->window)"), std::string::npos);

  const std::size_t button_up = source.find("case WM_LBUTTONUP:", mouse_move);
  ASSERT_NE(button_up, std::string::npos);
  const std::string move_block =
      source.substr(mouse_move, button_up - mouse_move);
  EXPECT_NE(move_block.find(".move_to("), std::string::npos);
  EXPECT_NE(move_block.find("has_value()"), std::string::npos);
  EXPECT_NE(move_block.find("transform.pan_by"), std::string::npos);

  const std::size_t key_down = source.find("case WM_KEYDOWN:", button_up);
  ASSERT_NE(key_down, std::string::npos);
  const std::string end_block = source.substr(button_up, key_down - button_up);
  EXPECT_NE(end_block.find("end_pan()"), std::string::npos);
  EXPECT_NE(source.find("pan.end()"), std::string::npos);
  EXPECT_NE(source.find("ReleaseCapture()"), std::string::npos);
}

TEST(AsyncLoadContractTest, LoadResultsCarryMetricsAndPath) {
  const std::string source = main_window_source();

  EXPECT_NE(source.find("#include \"viewer/core/load_metrics.h\""),
            std::string::npos);
  EXPECT_NE(source.find("core::LoadMetrics metrics"), std::string::npos);
  EXPECT_NE(source.find(".requested = core::LoadMetrics::Clock::now()"),
            std::string::npos);
  EXPECT_NE(source.find("metrics.decode_started = "
                        "core::LoadMetrics::Clock::now()"),
            std::string::npos);
  EXPECT_NE(source.find("metrics.decode_finished = "
                        "core::LoadMetrics::Clock::now()"),
            std::string::npos);
  EXPECT_NE(source.find("pending_load_debug"), std::string::npos);
}

TEST(AsyncLoadContractTest, DecodeFailuresShowPublicStatusAndLogDetails) {
  const std::string source = main_window_source();

  EXPECT_NE(source.find("This image could not be opened."), std::string::npos);
  EXPECT_NE(source.find("renderer.set_status_text"), std::string::npos);
  EXPECT_NE(source.find("OutputDebugStringW"), std::string::npos);
  EXPECT_NE(source.find("decode_us="), std::string::npos);
  EXPECT_NE(source.find("total_us="), std::string::npos);
  EXPECT_NE(source.find("pending.path.wstring()"), std::string::npos);
}

TEST(AsyncLoadContractTest, LoadedImageHandlingDefersMetricsUntilPaint) {
  const std::string source = main_window_source();

  const std::size_t image_ready = source.find("case image_ready_message:");
  ASSERT_NE(image_ready, std::string::npos);
  const std::size_t wheel = source.find("case WM_MOUSEWHEEL:", image_ready);
  ASSERT_NE(wheel, std::string::npos);
  const std::string image_ready_block =
      source.substr(image_ready, wheel - image_ready);

  EXPECT_EQ(image_ready_block.find(".presented = "
                                   "core::LoadMetrics::Clock::now()"),
            std::string::npos);
  EXPECT_EQ(image_ready_block.find("OutputDebugStringW"), std::string::npos);
  EXPECT_EQ(image_ready_block.find("output_load_debug_string"),
            std::string::npos);
  EXPECT_NE(image_ready_block.find("pending_load_debug ="),
            std::string::npos);
  EXPECT_NE(image_ready_block.find("InvalidateRect"), std::string::npos);
}

TEST(AsyncLoadContractTest, PaintFlushesPendingMetricsAfterSuccessfulDraw) {
  const std::string source = main_window_source();

  const std::size_t paint = source.find("case WM_PAINT:");
  ASSERT_NE(paint, std::string::npos);
  const std::size_t image_ready = source.find("case image_ready_message:",
                                             paint);
  ASSERT_NE(image_ready, std::string::npos);
  const std::string paint_block = source.substr(paint, image_ready - paint);

  EXPECT_NE(paint_block.find("bool draw_succeeded = false"),
            std::string::npos);
  EXPECT_NE(paint_block.find("draw_succeeded = draw_result.value()"),
            std::string::npos);
  EXPECT_EQ(paint_block.find("draw_succeeded = true"), std::string::npos);
  EXPECT_NE(paint_block.find("EndPaint"), std::string::npos);
  EXPECT_NE(paint_block.find("pending_load_debug.has_value()"),
            std::string::npos);
  EXPECT_NE(paint_block.find(".metrics.presented = "
                             "core::LoadMetrics::Clock::now()"),
            std::string::npos);
  EXPECT_NE(paint_block.find("output_load_debug_string"),
            std::string::npos);
  EXPECT_NE(paint_block.find("pending_load_debug.reset()"),
            std::string::npos);

  const std::size_t draw_error = paint_block.find("if (draw_error.has_value())");
  ASSERT_NE(draw_error, std::string::npos);
  const std::size_t draw_success = paint_block.find("if (draw_succeeded",
                                                    draw_error);
  ASSERT_NE(draw_success, std::string::npos);
  const std::string error_block =
      paint_block.substr(draw_error, draw_success - draw_error);
  EXPECT_EQ(error_block.find("pending_load_debug.reset()"),
            std::string::npos);
  EXPECT_EQ(error_block.find("output_load_debug_string"),
            std::string::npos);
}

TEST(AsyncLoadContractTest, PaintKeepsPendingMetricsWhenDrawDoesNotPresent) {
  const std::string source = main_window_source();

  const std::size_t paint = source.find("case WM_PAINT:");
  ASSERT_NE(paint, std::string::npos);
  const std::size_t image_ready = source.find("case image_ready_message:",
                                             paint);
  ASSERT_NE(image_ready, std::string::npos);
  const std::string paint_block = source.substr(paint, image_ready - paint);

  EXPECT_NE(paint_block.find("draw_succeeded = draw_result.value()"),
            std::string::npos);
  EXPECT_NE(paint_block.find("draw_succeeded && "
                             "impl_->pending_load_debug.has_value()"),
            std::string::npos);
  EXPECT_EQ(paint_block.find("draw_result.has_value() && "
                             "impl_->pending_load_debug.has_value()"),
            std::string::npos);
}

TEST(AsyncLoadContractTest, RenderTargetLossAttemptsRecoveryWithoutQuit) {
  const std::string source = main_window_source();

  EXPECT_NE(source.find("bool recovering_renderer = false"), std::string::npos);
  EXPECT_NE(source.find("bool try_recover_renderer()"), std::string::npos);
  EXPECT_NE(source.find("error.code == core::ErrorCode::render_target_lost"),
            std::string::npos);

  const std::size_t recovery = source.find("bool try_recover_renderer()");
  ASSERT_NE(recovery, std::string::npos);
  const std::size_t report = source.find("void report_renderer_failure(",
                                         recovery);
  ASSERT_NE(report, std::string::npos);
  const std::string recovery_block = source.substr(recovery, report - recovery);

  EXPECT_NE(recovery_block.find("renderer.initialize(window)"),
            std::string::npos);
  EXPECT_NE(recovery_block.find("renderer.set_image(*displayed_frame)"),
            std::string::npos);
  EXPECT_NE(recovery_block.find("renderer.set_status_text("),
            std::string::npos);
  EXPECT_NE(recovery_block.find("InvalidateRect(window, nullptr, FALSE)"),
            std::string::npos);
  EXPECT_EQ(recovery_block.find("PostQuitMessage"), std::string::npos);
  EXPECT_EQ(recovery_block.find("MessageBox"), std::string::npos);

  const std::size_t reset_failed =
      source.find("void show_renderer_reset_failed()", recovery);
  ASSERT_NE(reset_failed, std::string::npos);
  const std::string reset_failed_block =
      source.substr(reset_failed, report - reset_failed);
  EXPECT_NE(reset_failed_block.find("renderer.clear_image()"),
            std::string::npos);
  EXPECT_NE(reset_failed_block.find("set_renderer_status_text("),
            std::string::npos);
  EXPECT_NE(reset_failed_block.find("Renderer reset failed."),
            std::string::npos);

  const std::size_t report_end =
      source.find("void discard_pending_completions()", report);
  ASSERT_NE(report_end, std::string::npos);
  const std::string report_block = source.substr(report, report_end - report);

  EXPECT_NE(report_block.find("try_recover_renderer()"), std::string::npos);
  EXPECT_NE(report_block.find("show_renderer_reset_failed()"),
            std::string::npos);

  const std::size_t lost_branch =
      report_block.find("error.code == core::ErrorCode::render_target_lost");
  ASSERT_NE(lost_branch, std::string::npos);
  const std::size_t fatal_branch =
      report_block.find("renderer_ready = false", lost_branch);
  ASSERT_NE(fatal_branch, std::string::npos);
  const std::string lost_block =
      report_block.substr(lost_branch, fatal_branch - lost_branch);
  EXPECT_EQ(lost_block.find("PostQuitMessage"), std::string::npos);
  EXPECT_EQ(lost_block.find("MessageBox"), std::string::npos);
}

TEST(AsyncLoadContractTest, RenderTargetLossCallSitesRetryOrInvalidate) {
  const std::string source = main_window_source();

  EXPECT_NE(source.find("handle_resize_result(width, height, resize_result)"),
            std::string::npos);
  EXPECT_NE(source.find("handle_upload_result(upload_result, frame)"),
            std::string::npos);
  EXPECT_NE(source.find("handle_draw_error(*draw_error)"), std::string::npos);

  const std::size_t resize = source.find("bool handle_resize_result(");
  ASSERT_NE(resize, std::string::npos);
  const std::size_t upload = source.find("bool handle_upload_result(", resize);
  ASSERT_NE(upload, std::string::npos);
  const std::string resize_block = source.substr(resize, upload - resize);
  EXPECT_NE(resize_block.find("try_recover_renderer()"), std::string::npos);
  EXPECT_NE(resize_block.find("renderer.resize(width, height)"),
            std::string::npos);

  const std::size_t draw = source.find("void handle_draw_error(", upload);
  ASSERT_NE(draw, std::string::npos);
  const std::string upload_block = source.substr(upload, draw - upload);
  EXPECT_NE(upload_block.find("try_recover_renderer()"), std::string::npos);
  EXPECT_NE(upload_block.find("renderer.set_image(*frame)"),
            std::string::npos);

  const std::size_t report = source.find("void report_renderer_failure(",
                                         draw);
  ASSERT_NE(report, std::string::npos);
  const std::string draw_block = source.substr(draw, report - draw);
  EXPECT_NE(draw_block.find("try_recover_renderer()"), std::string::npos);
  EXPECT_NE(draw_block.find("InvalidateRect(window, nullptr, FALSE)"),
            std::string::npos);
  EXPECT_EQ(draw_block.find("PostQuitMessage"), std::string::npos);
  EXPECT_EQ(draw_block.find("MessageBox"), std::string::npos);
}

}  // namespace
