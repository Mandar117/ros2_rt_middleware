#include <gtest/gtest.h>

#include <set>
#include <thread>
#include <vector>

#include "memory_pool.hpp"

using rt_middleware::MemoryPool;

struct Widget {
  int   id;
  float value;
};

// ── Capacity and availability ─────────────────────────────────────────────────

TEST(MemoryPool, AvailableStartsAtCapacity) {
  MemoryPool<Widget, 32> pool;
  EXPECT_EQ(pool.available(), 32u);
  EXPECT_EQ(pool.capacity(), 32u);
}

TEST(MemoryPool, AllocateReducesAvailable) {
  MemoryPool<Widget, 8> pool;
  auto* p = pool.allocate();
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(pool.available(), 7u);
  new (p) Widget{1, 1.0f};
  p->~Widget();
  pool.deallocate(p);
}

TEST(MemoryPool, ExhaustionReturnsNull) {
  MemoryPool<Widget, 4> pool;
  Widget* ptrs[4];
  for (int i = 0; i < 4; ++i) {
    ptrs[i] = pool.allocate();
    ASSERT_NE(ptrs[i], nullptr) << "Allocation " << i << " should succeed";
  }
  EXPECT_EQ(pool.available(), 0u);
  EXPECT_EQ(pool.allocate(), nullptr) << "5th allocation should fail";

  // Clean up.
  for (auto* p : ptrs) pool.deallocate(p);
}

TEST(MemoryPool, DeallocateRestoresAvailability) {
  MemoryPool<Widget, 4> pool;
  Widget* p1 = pool.allocate();
  Widget* p2 = pool.allocate();
  EXPECT_EQ(pool.available(), 2u);
  pool.deallocate(p1);
  EXPECT_EQ(pool.available(), 3u);
  pool.deallocate(p2);
  EXPECT_EQ(pool.available(), 4u);
}

// ── Pointer uniqueness and validity ──────────────────────────────────────────

TEST(MemoryPool, AllPointersAreUnique) {
  MemoryPool<Widget, 64> pool;
  std::set<Widget*> seen;
  for (std::size_t i = 0; i < 64; ++i) {
    Widget* p = pool.allocate();
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(seen.insert(p).second) << "Duplicate pointer at slot " << i;
  }
  for (Widget* p : seen) pool.deallocate(p);
}

TEST(MemoryPool, WrittenDataSurvivesUntilDeallocate) {
  MemoryPool<Widget, 8> pool;
  Widget* p = pool.allocate();
  ASSERT_NE(p, nullptr);
  new (p) Widget{42, 3.14f};
  EXPECT_EQ(p->id, 42);
  EXPECT_FLOAT_EQ(p->value, 3.14f);
  p->~Widget();
  pool.deallocate(p);
}

// ── Recycle: allocate → free → allocate ──────────────────────────────────────

TEST(MemoryPool, RecycleAfterExhaustion) {
  MemoryPool<Widget, 2> pool;
  Widget* a = pool.allocate();
  Widget* b = pool.allocate();
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(pool.allocate(), nullptr);

  pool.deallocate(a);
  Widget* c = pool.allocate();
  ASSERT_NE(c, nullptr);  // recycled from a
  EXPECT_EQ(pool.allocate(), nullptr);

  pool.deallocate(b);
  pool.deallocate(c);
  EXPECT_EQ(pool.available(), 2u);
}

TEST(MemoryPool, NullDeallocateIsNoop) {
  MemoryPool<Widget, 4> pool;
  EXPECT_NO_THROW(pool.deallocate(nullptr));
  EXPECT_EQ(pool.available(), 4u);
}

// ── Concurrent allocate/deallocate ────────────────────────────────────────────

TEST(MemoryPool, ConcurrentAllocDeallocStressTest) {
  static constexpr int kThreads = 4;
  static constexpr int kRounds  = 10'000;
  MemoryPool<Widget, 128> pool;

  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&pool, t] {
      for (int r = 0; r < kRounds; ++r) {
        Widget* p = pool.allocate();
        if (p) {
          new (p) Widget{t * 1000 + r, static_cast<float>(r)};
          // Hold briefly to increase contention.
          p->~Widget();
          pool.deallocate(p);
        }
        // Intentional: some iterations may see nullptr due to contention.
      }
    });
  }

  for (auto& th : threads) th.join();

  // All slots should be returned.
  EXPECT_EQ(pool.available(), 128u);
}
