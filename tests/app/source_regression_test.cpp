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
     ThumbnailImageOpenCopiesEntryPathBeforeClearingThumbnailCache) {
  const std::string source = read_main_window_source();
  ASSERT_FALSE(source.empty());

  const std::string expected =
      "case ThumbnailBrowserEntryKind::image:\n"
      "        const std::filesystem::path selected_path = entry.path;\n"
      "        clear_thumbnail_browser_directory();\n"
      "        open_path(selected_path);";

  EXPECT_NE(source.find(expected), std::string::npos);
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
