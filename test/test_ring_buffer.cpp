#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "lock_free_ring_buffer.hpp"

using rt_middleware::LockFreeRingBuffer;

// ── Basic single-threaded correctness ─────────────────────────────────────────

TEST(RingBuffer, EmptyOnConstruction) {
  LockFreeRingBuffer<int, 8> rb;
  EXPECT_TRUE(rb.empty());
  EXPECT_EQ(rb.size_approx(), 0u);
}

TEST(RingBuffer, PushAndPop) {
  LockFreeRingBuffer<int, 8> rb;
  EXPECT_TRUE(rb.push(42));
  EXPECT_FALSE(rb.empty());

  int val = 0;
  EXPECT_TRUE(rb.pop(val));
  EXPECT_EQ(val, 42);
  EXPECT_TRUE(rb.empty());
}

TEST(RingBuffer, FifoOrdering) {
  LockFreeRingBuffer<int, 16> rb;
  for (int i = 0; i < 10; ++i) {
    EXPECT_TRUE(rb.push(i));
  }
  for (int i = 0; i < 10; ++i) {
    int val = -1;
    EXPECT_TRUE(rb.pop(val));
    EXPECT_EQ(val, i) << "FIFO violated at position " << i;
  }
}

TEST(RingBuffer, PopFromEmptyReturnsFalse) {
  LockFreeRingBuffer<int, 8> rb;
  int val = 0;
  EXPECT_FALSE(rb.pop(val));
}

TEST(RingBuffer, PushToFullReturnsFalse) {
  // Capacity=8 means max_size()=7 elements.
  LockFreeRingBuffer<int, 8> rb;
  for (std::size_t i = 0; i < rb.max_size(); ++i) {
    EXPECT_TRUE(rb.push(static_cast<int>(i))) << "Should succeed at slot " << i;
  }
  EXPECT_FALSE(rb.push(999)) << "Push beyond max_size should fail";
}

TEST(RingBuffer, MaxSizeIsCapacityMinusOne) {
  EXPECT_EQ((LockFreeRingBuffer<int, 8>::max_size()), 7u);
  EXPECT_EQ((LockFreeRingBuffer<int, 64>::max_size()), 63u);
}

TEST(RingBuffer, WrapAround) {
  LockFreeRingBuffer<int, 4> rb;  // max 3 slots

  // Fill, drain, fill again — exercises wrap-around.
  for (int round = 0; round < 5; ++round) {
    for (int i = 0; i < 3; ++i) {
      EXPECT_TRUE(rb.push(i + round * 100));
    }
    for (int i = 0; i < 3; ++i) {
      int val = -1;
      EXPECT_TRUE(rb.pop(val));
      EXPECT_EQ(val, i + round * 100);
    }
    EXPECT_TRUE(rb.empty());
  }
}

TEST(RingBuffer, SizeApprox) {
  LockFreeRingBuffer<int, 16> rb;
  EXPECT_EQ(rb.size_approx(), 0u);
  rb.push(1);
  rb.push(2);
  rb.push(3);
  EXPECT_EQ(rb.size_approx(), 3u);
  int v;
  rb.pop(v);
  EXPECT_EQ(rb.size_approx(), 2u);
}

TEST(RingBuffer, MoveOnlyType) {
  LockFreeRingBuffer<std::unique_ptr<int>, 8> rb;
  EXPECT_TRUE(rb.push(std::make_unique<int>(7)));
  std::unique_ptr<int> out;
  EXPECT_TRUE(rb.pop(out));
  EXPECT_NE(out, nullptr);
  EXPECT_EQ(*out, 7);
}

// ── Concurrent SPSC correctness ───────────────────────────────────────────────

TEST(RingBuffer, SpscThroughput) {
  static constexpr int kItems = 100'000;
  LockFreeRingBuffer<int, 1024> rb;

  std::atomic<bool> start{false};
  std::atomic<int>  received{0};

  std::thread producer([&] {
    while (!start.load(std::memory_order_acquire)) {}
    for (int i = 0; i < kItems; ++i) {
      while (!rb.push(i)) {
        // Spin until space is available.
      }
    }
  });

  std::thread consumer([&] {
    while (!start.load(std::memory_order_acquire)) {}
    int val = 0;
    int next_expected = 0;
    while (next_expected < kItems) {
      if (rb.pop(val)) {
        EXPECT_EQ(val, next_expected) << "FIFO order violated";
        ++next_expected;
        received.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  start.store(true, std::memory_order_release);
  producer.join();
  consumer.join();

  EXPECT_EQ(received.load(), kItems);
}
