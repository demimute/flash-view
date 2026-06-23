#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>

#include "viewer/core/image_frame.h"
#include "viewer/core/three_frame_cache.h"

namespace viewer::core {
namespace {

std::shared_ptr<ImageFrame> frame(std::uint32_t width) {
  auto image = std::make_shared<ImageFrame>();
  image->width = width;
  image->height = 1;
  image->pixels.resize(width * 4);
  return image;
}

TEST(ThreeFrameCacheTest, KeepsOnlyCurrentPreviousAndNextFrames) {
  ThreeFrameCache cache;
  const auto current = std::filesystem::path{"current.png"};
  const auto previous = std::filesystem::path{"previous.png"};
  const auto next = std::filesystem::path{"next.png"};
  const auto unrelated = std::filesystem::path{"unrelated.png"};

  cache.remember(unrelated, frame(4));
  cache.remember(current, frame(1));
  cache.remember(previous, frame(2));
  cache.remember(next, frame(3));
  cache.retain(current, previous, next);

  EXPECT_NE(cache.find(current), nullptr);
  EXPECT_NE(cache.find(previous), nullptr);
  EXPECT_NE(cache.find(next), nullptr);
  EXPECT_EQ(cache.find(unrelated), nullptr);
}

TEST(ThreeFrameCacheTest, ReplacesFramesOutsideThreeFrameWindow) {
  ThreeFrameCache cache;
  const auto old_previous = std::filesystem::path{"old_previous.png"};
  const auto current = std::filesystem::path{"current.png"};
  const auto previous = std::filesystem::path{"previous.png"};
  const auto next = std::filesystem::path{"next.png"};

  cache.remember(old_previous, frame(9));
  cache.remember(current, frame(1));
  cache.remember(previous, frame(2));
  cache.remember(next, frame(3));
  cache.retain(current, previous, next);

  EXPECT_EQ(cache.find(old_previous), nullptr);
  ASSERT_NE(cache.find(previous), nullptr);
  EXPECT_EQ(cache.find(previous)->width, 2U);
}

TEST(ThreeFrameCacheTest, PrefetchTrackerDeduplicatesInFlightAndCachedPaths) {
  ThreeFrameCache cache;
  PrefetchTracker tracker;
  const auto current = std::filesystem::path{"current.png"};
  const auto next = std::filesystem::path{"next.png"};

  cache.remember(current, frame(1));

  EXPECT_FALSE(tracker.should_submit(current, cache));
  EXPECT_TRUE(tracker.should_submit(next, cache));
  EXPECT_FALSE(tracker.should_submit(next, cache));

  tracker.finish(next);
  EXPECT_TRUE(tracker.should_submit(next, cache));
}

TEST(ThreeFrameCacheTest, CurrentStaleDependsOnGenerationButPrefetchStaleDependsOnRelevance) {
  const auto current = std::filesystem::path{"current.png"};
  const auto previous = std::filesystem::path{"previous.png"};
  const auto next = std::filesystem::path{"next.png"};
  const auto unrelated = std::filesystem::path{"unrelated.png"};

  EXPECT_TRUE(should_accept_loaded_image(
      LoadedImagePurpose::display, current, current, previous, next,
      /*request_generation=*/7, /*current_generation=*/7));
  EXPECT_FALSE(should_accept_loaded_image(
      LoadedImagePurpose::display, current, current, previous, next,
      /*request_generation=*/6, /*current_generation=*/7));
  EXPECT_TRUE(should_accept_loaded_image(
      LoadedImagePurpose::prefetch, previous, current, previous, next,
      /*request_generation=*/6, /*current_generation=*/7));
  EXPECT_FALSE(should_accept_loaded_image(
      LoadedImagePurpose::prefetch, unrelated, current, previous, next,
      /*request_generation=*/6, /*current_generation=*/7));
}

}  // namespace
}  // namespace viewer::core
