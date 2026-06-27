#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string read_main_window_source() {
  const std::filesystem::path source =
      std::filesystem::path{PROJECT_SOURCE_DIR} / "src" / "app" /
      "main_window.cpp";
  std::ifstream stream(source);
  std::string contents((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
  return contents;
}

}  // namespace

TEST(SourceRegressionTest,
     ThumbnailImageOpenCopiesEntryPathBeforeOpeningImage) {
  const std::string source = read_main_window_source();
  ASSERT_FALSE(source.empty());

  const std::string expected =
      "case ThumbnailBrowserEntryKind::image:\n"
      "        const std::filesystem::path selected_path = entry.path;\n"
      "        open_path(selected_path);";

  EXPECT_NE(source.find(expected), std::string::npos);
}

TEST(SourceRegressionTest, ThumbnailRenderingNeverSynchronouslyDecodesImages) {
  const std::string source = read_main_window_source();
  ASSERT_FALSE(source.empty());

  EXPECT_EQ(source.find(": thumbnail_frame_for(entry.path)"),
            std::string::npos);
  EXPECT_NE(source.find("request_thumbnail(entry.path"), std::string::npos);
}

TEST(SourceRegressionTest, ThumbnailCacheLookupDoesNotReturnFullSizeFrames) {
  const std::string source = read_main_window_source();
  ASSERT_FALSE(source.empty());

  const std::size_t function_start =
      source.find("cached_thumbnail_frame_for(");
  ASSERT_NE(function_start, std::string::npos);
  const std::size_t function_end =
      source.find("[[nodiscard]] std::shared_ptr<core::ImageFrame> thumbnail_frame_for",
                  function_start);
  ASSERT_NE(function_end, std::string::npos);
  const std::string function_body =
      source.substr(function_start, function_end - function_start);

  EXPECT_EQ(function_body.find("frame_cache.find"), std::string::npos);
}

TEST(SourceRegressionTest, OpeningImageKeepsThumbnailPaneEntriesStable) {
  const std::string source = read_main_window_source();
  ASSERT_FALSE(source.empty());

  const std::string forbidden =
      "navigator.reset();\n"
      "    clear_thumbnail_browser_directory();\n"
      "    thumbnail_entries_cache.clear();";

  EXPECT_EQ(source.find(forbidden), std::string::npos);
}

TEST(SourceRegressionTest, ThumbnailOverlayIteratesVisibleRangeOnly) {
  const std::string source = read_main_window_source();
  ASSERT_FALSE(source.empty());

  EXPECT_NE(source.find("const std::size_t first_index"),
            std::string::npos);
  EXPECT_NE(source.find("const std::size_t end_index"), std::string::npos);
  EXPECT_NE(source.find("for (std::size_t index = first_index; index < end_index; ++index)"),
            std::string::npos);
}

TEST(SourceRegressionTest, ThumbnailLoadingIsThrottledAndBatched) {
  const std::string source = read_main_window_source();
  ASSERT_FALSE(source.empty());

  EXPECT_NE(source.find("max_thumbnail_requests_in_flight"), std::string::npos);
  EXPECT_NE(source.find("cancel_pending_thumbnail_requests()"),
            std::string::npos);
  EXPECT_NE(source.find("thumbnail_refresh_timer_id"), std::string::npos);
  EXPECT_NE(source.find("schedule_thumbnail_refresh()"), std::string::npos);
  EXPECT_EQ(source.find("InvalidateRect(impl_->window, nullptr, FALSE);\n      }"
                        "\n      return 0;\n    }\n\n    case WM_MOUSEWHEEL"),
            std::string::npos);
}

TEST(SourceRegressionTest, RendererCachesUploadedThumbnailBitmaps) {
  const std::filesystem::path source_path =
      std::filesystem::path{PROJECT_SOURCE_DIR} / "src" / "render" /
      "d3d_renderer.cpp";
  std::ifstream stream(source_path);
  const std::string source((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
  ASSERT_FALSE(source.empty());

  EXPECT_NE(source.find("thumbnail_bitmaps"), std::string::npos);
  EXPECT_NE(source.find("thumbnail_bitmap_for"), std::string::npos);
  EXPECT_NE(source.find("thumbnail_bitmaps.emplace"), std::string::npos);
}

TEST(SourceRegressionTest, AssociationToolDirectlyAssignsImageExtensions) {
  const std::filesystem::path source_path =
      std::filesystem::path{PROJECT_SOURCE_DIR} / "src" / "tools" /
      "file_association.cpp";
  std::ifstream stream(source_path);
  const std::string source((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
  ASSERT_FALSE(source.empty());

  EXPECT_NE(source.find("delete_user_choice(extension);"), std::string::npos);
  EXPECT_NE(source.find("write_key_default(extension_key, std::wstring(prog_id))"),
            std::string::npos);
}

TEST(SourceRegressionTest, AssociationToolDetectsWindowsDefaultAppBlocking) {
  const std::filesystem::path source_path =
      std::filesystem::path{PROJECT_SOURCE_DIR} / "src" / "tools" /
      "file_association.cpp";
  std::ifstream stream(source_path);
  const std::string source((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
  ASSERT_FALSE(source.empty());

  EXPECT_NE(source.find("user_choice_prog_id"), std::string::npos);
  EXPECT_NE(source.find("blocked_extensions"), std::string::npos);
  EXPECT_NE(source.find("SystemFileAssociations\\\\image\\\\shell\\\\FlashView"),
            std::string::npos);
  EXPECT_NE(source.find("Windows 10/11 default-app protection"),
            std::string::npos);
}

TEST(SourceRegressionTest, AssociationToolChecksEffectiveWindowsDefault) {
  const std::filesystem::path source_path =
      std::filesystem::path{PROJECT_SOURCE_DIR} / "src" / "tools" /
      "file_association.cpp";
  std::ifstream stream(source_path);
  const std::string source((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
  ASSERT_FALSE(source.empty());

  EXPECT_NE(source.find("SetAppAsDefault"), std::string::npos);
  EXPECT_NE(source.find("QueryCurrentDefault"), std::string::npos);
  EXPECT_NE(source.find("AL_EFFECTIVE"), std::string::npos);
}

TEST(SourceRegressionTest, AssociationUninstallRemovesExplorerOpenWithEntries) {
  const std::filesystem::path source_path =
      std::filesystem::path{PROJECT_SOURCE_DIR} / "src" / "tools" /
      "file_association.cpp";
  std::ifstream stream(source_path);
  const std::string source((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
  ASSERT_FALSE(source.empty());

  EXPECT_NE(source.find("Explorer\\\\FileExts\\\\"), std::string::npos);
  EXPECT_NE(source.find("delete_explorer_open_with_list_entries"),
            std::string::npos);
  EXPECT_NE(source.find("delete_value(explorer_extension_key(extension) + "
                        "L\"\\\\OpenWithProgids\", prog_id)"),
            std::string::npos);
}

TEST(SourceRegressionTest, PublicExecutableNameIsFlashView) {
  const std::filesystem::path tool_source_path =
      std::filesystem::path{PROJECT_SOURCE_DIR} / "src" / "tools" /
      "file_association.cpp";
  std::ifstream tool_stream(tool_source_path);
  const std::string tool_source((std::istreambuf_iterator<char>(tool_stream)),
                                std::istreambuf_iterator<char>());
  ASSERT_FALSE(tool_source.empty());

  EXPECT_NE(tool_source.find("FlashView.exe"), std::string::npos);
  EXPECT_EQ(tool_source.find("fast_viewer.exe"), std::string::npos);

  const std::filesystem::path cmake_path =
      std::filesystem::path{PROJECT_SOURCE_DIR} / "CMakeLists.txt";
  std::ifstream cmake_stream(cmake_path);
  const std::string cmake_source((std::istreambuf_iterator<char>(cmake_stream)),
                                 std::istreambuf_iterator<char>());
  ASSERT_FALSE(cmake_source.empty());
  EXPECT_NE(cmake_source.find("OUTPUT_NAME FlashView"), std::string::npos);
}
