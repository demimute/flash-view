#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "viewer/core/load_generation.h"

namespace viewer::core {
namespace {

TEST(LoadGenerationTest, StartsAtZero) {
  LoadGeneration generation;

  EXPECT_TRUE(generation.is_current(0));
}

TEST(LoadGenerationTest, OnlyAcceptsLatestGeneration) {
  LoadGeneration generation;

  const std::uint64_t first = generation.begin();
  const std::uint64_t second = generation.begin();

  EXPECT_FALSE(generation.is_current(first));
  EXPECT_TRUE(generation.is_current(second));
}

TEST(LoadGenerationTest, ConcurrentBeginsProduceOneCurrentGeneration) {
  constexpr std::size_t thread_count = 8;
  constexpr std::size_t begins_per_thread = 10'000;

  LoadGeneration generation;
  std::mutex values_mutex;
  std::vector<std::uint64_t> values;
  values.reserve(thread_count * begins_per_thread);
  std::vector<std::thread> threads;
  threads.reserve(thread_count);

  for (std::size_t thread_index = 0; thread_index < thread_count;
       ++thread_index) {
    threads.emplace_back([&] {
      std::vector<std::uint64_t> local_values;
      local_values.reserve(begins_per_thread);
      for (std::size_t index = 0; index < begins_per_thread; ++index) {
        local_values.push_back(generation.begin());
      }

      std::scoped_lock lock(values_mutex);
      values.insert(values.end(), local_values.begin(), local_values.end());
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  std::sort(values.begin(), values.end());
  ASSERT_EQ(values.size(), thread_count * begins_per_thread);
  EXPECT_EQ(values.front(), 1U);
  EXPECT_EQ(values.back(), thread_count * begins_per_thread);
  EXPECT_EQ(std::adjacent_find(values.begin(), values.end()), values.end());

  for (const std::uint64_t value : values) {
    EXPECT_EQ(generation.is_current(value), value == values.back());
  }
}

}  // namespace
}  // namespace viewer::core
