#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "viewer/core/async_shutdown.h"

namespace viewer::core {
namespace {

struct TrackedPayload {
  explicit TrackedPayload(std::atomic_int& destructions)
      : destructions(destructions) {}

  ~TrackedPayload() { ++destructions; }

  std::atomic_int& destructions;
};

TEST(CompletionGateTest, CloseRejectsPublishAndPreservesPayloadOwnership) {
  CompletionGate<int, std::uint64_t> gate(42, 7);
  std::atomic_int destructions{0};
  auto payload = std::make_unique<TrackedPayload>(destructions);

  EXPECT_TRUE(gate.close());
  EXPECT_FALSE(gate.publish(payload, [](int, std::uint64_t, auto*) {
    return true;
  }));
  EXPECT_NE(payload, nullptr);
  EXPECT_EQ(destructions, 0);

  payload.reset();
  EXPECT_EQ(destructions, 1);
}

TEST(CompletionGateTest, PublishAndCloseAreMutuallyExclusive) {
  constexpr int iterations = 2'000;

  for (int iteration = 0; iteration < iterations; ++iteration) {
    CompletionGate<int, std::uint64_t> gate(42, 7);
    std::atomic_int published{0};
    std::atomic_bool close_returned{false};
    std::atomic_bool published_after_close{false};
    std::atomic_int destructions{0};
    TrackedPayload* queued_payload = nullptr;
    auto payload = std::make_unique<TrackedPayload>(destructions);

    std::thread publisher([&] {
      const bool accepted = gate.publish(
          payload, [&](int destination, std::uint64_t token,
                       TrackedPayload* published_payload) {
            EXPECT_EQ(destination, 42);
            EXPECT_EQ(token, 7U);
            if (close_returned.load(std::memory_order_acquire)) {
              published_after_close.store(true, std::memory_order_release);
            }
            queued_payload = published_payload;
            ++published;
            return true;
          });
      EXPECT_EQ(accepted, queued_payload != nullptr);
    });
    std::thread closer([&] {
      static_cast<void>(gate.close());
      close_returned.store(true, std::memory_order_release);
    });

    publisher.join();
    closer.join();

    EXPECT_FALSE(published_after_close.load(std::memory_order_acquire));
    EXPECT_LE(published.load(), 1);
    EXPECT_TRUE(gate.is_closed());
    if (payload) {
      payload.reset();
    } else {
      delete queued_payload;
    }
    EXPECT_EQ(destructions, 1);
  }
}

TEST(AsyncShutdownStateTest, ContextCanOnlyBeHandedOffOnce) {
  auto context = std::make_shared<int>(42);
  AsyncShutdownState<int> shutdown(context);

  const auto first = shutdown.take_context();
  const auto second = shutdown.take_context();

  ASSERT_NE(first, nullptr);
  EXPECT_EQ(*first, 42);
  EXPECT_EQ(second, nullptr);
}

TEST(ShutdownHandoffTest, PreallocatedPayloadCanOnlyBeTakenOnce) {
  auto handoff =
      ShutdownHandoff<int>(std::make_unique<int>(42));

  int* const first = handoff.take();
  int* const second = handoff.take();

  ASSERT_NE(first, nullptr);
  EXPECT_EQ(*first, 42);
  EXPECT_EQ(second, nullptr);
  delete first;
}

TEST(ShutdownHandoffTest, TakeIsNoexcept) {
  ShutdownHandoff<int> handoff(std::make_unique<int>(42));

  static_assert(noexcept(handoff.take()));
}

}  // namespace
}  // namespace viewer::core
