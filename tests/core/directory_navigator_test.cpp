#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "viewer/core/directory_navigator.h"

namespace viewer::core {
namespace {

constexpr std::array jpeg_magic{
    std::byte{0xFF},
    std::byte{0xD8},
    std::byte{0xFF},
    std::byte{0xE0},
};
constexpr std::array png_magic{
    std::byte{0x89},
    std::byte{0x50},
    std::byte{0x4E},
    std::byte{0x47},
    std::byte{0x0D},
    std::byte{0x0A},
    std::byte{0x1A},
    std::byte{0x0A},
};
constexpr std::array bmp_magic{
    std::byte{0x42},
    std::byte{0x4D},
};
constexpr std::array text_bytes{
    std::byte{'n'},
    std::byte{'o'},
    std::byte{'t'},
    std::byte{'e'},
};

class UniqueTempDirectory {
 public:
  UniqueTempDirectory() {
    static std::atomic_uint64_t sequence{0};
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("viewer-directory-navigator-" + std::to_string(nonce) + "-" +
             std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }

  ~UniqueTempDirectory() {
    std::error_code error;
    std::filesystem::permissions(
        path_, std::filesystem::perms::owner_all,
        std::filesystem::perm_options::add, error);
    std::filesystem::remove_all(path_, error);
  }

  UniqueTempDirectory(const UniqueTempDirectory&) = delete;
  UniqueTempDirectory& operator=(const UniqueTempDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

void write_bytes(const std::filesystem::path& path,
                 std::span<const std::byte> bytes) {
  std::ofstream output(path, std::ios::binary);
  ASSERT_TRUE(output.is_open());
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(output.good());
}

std::vector<std::filesystem::path> filenames(
    const std::vector<std::filesystem::path>& paths) {
  std::vector<std::filesystem::path> result;
  result.reserve(paths.size());
  for (const auto& path : paths) {
    result.push_back(path.filename());
  }
  return result;
}

TEST(DirectoryNavigatorTest, ScansMagicImagesAndSortsNamesNaturally) {
  UniqueTempDirectory directory;
  write_bytes(directory.path() / "10.jpg", jpeg_magic);
  write_bytes(directory.path() / "2.png", png_magic);
  write_bytes(directory.path() / "notes.txt", text_bytes);

  const auto result = DirectoryNavigator::scan(directory.path() / "10.jpg");

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(filenames(result.value().items()),
            (std::vector<std::filesystem::path>{"2.png", "10.jpg"}));
  EXPECT_EQ(result.value().current_index(), 1U);
  EXPECT_EQ(result.value().current().filename(), "10.jpg");
}

TEST(DirectoryNavigatorTest, PreviousAndNextWrapFromUpdatedPosition) {
  UniqueTempDirectory directory;
  write_bytes(directory.path() / "10.jpg", jpeg_magic);
  write_bytes(directory.path() / "2.png", png_magic);

  auto result = DirectoryNavigator::scan(directory.path() / "2.png");

  ASSERT_TRUE(result.has_value());
  auto navigator = std::move(result).value();
  EXPECT_EQ(navigator.previous().filename(), "10.jpg");
  EXPECT_EQ(navigator.current_index(), 1U);
  EXPECT_EQ(navigator.next().filename(), "2.png");
  EXPECT_EQ(navigator.current_index(), 0U);
}

TEST(DirectoryNavigatorTest, RejectsMissingSelectedPath) {
  UniqueTempDirectory directory;

  const auto result = DirectoryNavigator::scan(directory.path() / "missing.jpg");

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ErrorCode::io_error);
}

TEST(DirectoryNavigatorTest, RejectsSelectedNonImage) {
  UniqueTempDirectory directory;
  write_bytes(directory.path() / "notes.txt", text_bytes);

  const auto result = DirectoryNavigator::scan(directory.path() / "notes.txt");

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ErrorCode::unsupported_format);
}

TEST(DirectoryNavigatorTest, ReportsIoErrorForEmptyDirectorySelection) {
  UniqueTempDirectory directory;

  const auto result = DirectoryNavigator::scan(directory.path());

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ErrorCode::io_error);
}

#ifndef _WIN32
TEST(DirectoryNavigatorTest, ReportsIoErrorWhenDirectoryCannotBeRead) {
  UniqueTempDirectory directory;
  const auto selected = directory.path() / "selected.jpg";
  write_bytes(selected, jpeg_magic);
  std::error_code permission_error;
  std::filesystem::permissions(
      directory.path(), std::filesystem::perms::none,
      std::filesystem::perm_options::replace, permission_error);
  if (permission_error) {
    GTEST_SKIP() << "Could not make temporary directory unreadable";
  }

  std::error_code iterator_error;
  std::filesystem::directory_iterator probe(directory.path(), iterator_error);
  if (!iterator_error) {
    GTEST_SKIP() << "Current user can still read permissionless directories";
  }

  const auto result = DirectoryNavigator::scan(selected);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ErrorCode::io_error);
}
#endif

TEST(DirectoryNavigatorTest, FiltersByContentInsteadOfExtension) {
  UniqueTempDirectory directory;
  write_bytes(directory.path() / "real-image.txt", bmp_magic);
  write_bytes(directory.path() / "fake-image.png", text_bytes);

  const auto result =
      DirectoryNavigator::scan(directory.path() / "real-image.txt");

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(filenames(result.value().items()),
            (std::vector<std::filesystem::path>{"real-image.txt"}));
}

TEST(DirectoryNavigatorTest, SingleItemNavigationStaysOnCurrentItem) {
  UniqueTempDirectory directory;
  write_bytes(directory.path() / "only.bin", png_magic);

  auto result = DirectoryNavigator::scan(directory.path() / "only.bin");

  ASSERT_TRUE(result.has_value());
  auto navigator = std::move(result).value();
  EXPECT_EQ(navigator.previous().filename(), "only.bin");
  EXPECT_EQ(navigator.next().filename(), "only.bin");
  EXPECT_EQ(navigator.current_index(), 0U);
}

}  // namespace
}  // namespace viewer::core
