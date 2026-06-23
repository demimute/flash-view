#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace viewer::render {
namespace {

std::string renderer_source() {
  const auto path = std::filesystem::path(TEST_SOURCE_DIR) /
                    "src/render/d3d_renderer.cpp";
  std::ifstream input(path, std::ios::binary);
  EXPECT_TRUE(input.is_open()) << path;
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::size_t occurrence_count(const std::string& text,
                             const std::string& pattern) {
  std::size_t count = 0;
  std::size_t position = 0;
  while ((position = text.find(pattern, position)) != std::string::npos) {
    ++count;
    position += pattern.size();
  }
  return count;
}

TEST(D3dRendererContractTest, UsesBitmapPropertiesHelperForBothBitmaps) {
  const std::string source = renderer_source();
  const std::string helper = "D2D1::BitmapProperties1(";
  const auto first = source.find(helper);

  ASSERT_NE(first, std::string::npos);
  EXPECT_NE(source.find(helper, first + helper.size()), std::string::npos);
  EXPECT_EQ(source.find("D2D1_BITMAP_PROPERTIES1 properties{"),
            std::string::npos);
}

TEST(D3dRendererContractTest, ClassifiesAndClearsLostRenderTargets) {
  const std::string source = renderer_source();

  EXPECT_NE(source.find("bool lost = false;"), std::string::npos);
  EXPECT_NE(source.find("ErrorCode::render_target_lost"), std::string::npos);
  EXPECT_NE(source.find("DXGI_ERROR_DRIVER_INTERNAL_ERROR"),
            std::string::npos);
  EXPECT_NE(source.find("mark_lost("), std::string::npos);
  EXPECT_NE(source.find("is_render_target_lost(draw_result)"),
            std::string::npos);
  EXPECT_GE(occurrence_count(source, "is_device_lost(result)"), 2U);
}

TEST(D3dRendererContractTest, ResizeAttemptsToRestoreTargetAfterFailure) {
  const std::string source = renderer_source();

  EXPECT_NE(source.find("const HRESULT recovery_result = "
                        "impl_->create_target();"),
            std::string::npos);
}

}  // namespace
}  // namespace viewer::render
