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

}  // namespace
