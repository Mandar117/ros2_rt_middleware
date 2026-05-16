#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "rt_executor.hpp"

using rt_middleware::RTExecutor;
using rt_middleware::RTExecutorConfig;
using rt_middleware::JitterStats;

// Helper: run the executor in a background thread for `duration`, then stop.
static std::thread run_async(RTExecutor& exec,
                             std::chrono::milliseconds duration) {
  return std::thread([&exec, duration] {
    std::thread stopper([&exec, duration] {
      std::this_thread::sleep_for(duration);
      exec.stop();
    });
    exec.spin();
    if (stopper.joinable()) stopper.join();
  });
}

// ── Enqueue and dispatch ──────────────────────────────────────────────────────

TEST(RTExecutor, SingleCallbackExecutes) {
  RTExecutorConfig cfg;
  cfg.cpu_core     = -1;   // no pinning — tests run on any core
  cfg.use_sched_fifo = false;  // avoid CAP_SYS_NICE requirement in CI
  cfg.busy_wait    = false;
  cfg.spin_period_ns = 100'000;  // 0.1 ms

  RTExecutor exec(cfg);
  std::atomic<bool> called{false};

  EXPECT_TRUE(exec.enqueue([&called] { called.store(true); }));

  auto th = run_async(exec, std::chrono::milliseconds(200));
  th.join();

  EXPECT_TRUE(called.load());
}

TEST(RTExecutor, MultipleCallbacksAllExecute) {
  RTExecutorConfig cfg;
  cfg.use_sched_fifo = false;
  cfg.busy_wait      = false;
  cfg.spin_period_ns = 100'000;

  RTExecutor exec(cfg);
  static constexpr int kN = 16;
  std::atomic<int> count{0};

  for (int i = 0; i < kN; ++i) {
    EXPECT_TRUE(exec.enqueue([&count] { count.fetch_add(1); }));
  }

  auto th = run_async(exec, std::chrono::milliseconds(300));
  th.join();

  EXPECT_EQ(count.load(), kN);
}

TEST(RTExecutor, FifoDispatchOrder) {
  RTExecutorConfig cfg;
  cfg.use_sched_fifo = false;
  cfg.busy_wait      = false;
  cfg.spin_period_ns = 100'000;

  RTExecutor exec(cfg);

  std::vector<int> order;
  std::mutex mtx;

  for (int i = 0; i < 8; ++i) {
    exec.enqueue([i, &order, &mtx] {
      std::lock_guard<std::mutex> lk(mtx);
      order.push_back(i);
    });
  }

  auto th = run_async(exec, std::chrono::milliseconds(300));
  th.join();

  ASSERT_EQ(static_cast<int>(order.size()), 8);
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(order[i], i) << "FIFO order violated at position " << i;
  }
}

// ── Stop behaviour ────────────────────────────────────────────────────────────

TEST(RTExecutor, StopHaltsSpinLoop) {
  RTExecutorConfig cfg;
  cfg.use_sched_fifo = false;
  cfg.busy_wait      = false;
  cfg.spin_period_ns = 1'000'000;  // 1 ms

  RTExecutor exec(cfg);

  const auto start = std::chrono::steady_clock::now();

  std::thread th([&exec] { exec.spin(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  exec.stop();
  th.join();

  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(elapsed, std::chrono::milliseconds(500))
      << "spin() did not return within 500 ms after stop()";
}

// ── Queue back-pressure ───────────────────────────────────────────────────────

TEST(RTExecutor, EnqueueReturnsFalseWhenQueueFull) {
  RTExecutorConfig cfg;
  cfg.use_sched_fifo = false;
  cfg.busy_wait      = false;

  RTExecutor exec(cfg);

  // Fill the queue without running the executor.
  int dropped = 0;
  for (std::size_t i = 0; i < RTExecutor::kQueueCapacity + 32; ++i) {
    if (!exec.enqueue([] {})) {
      ++dropped;
    }
  }
  EXPECT_GT(dropped, 0) << "Expected drops when queue is full";
  exec.stop();
}

// ── Jitter stats ──────────────────────────────────────────────────────────────

TEST(RTExecutor, JitterStatsNonZeroAfterCallbacks) {
  RTExecutorConfig cfg;
  cfg.use_sched_fifo = false;
  cfg.busy_wait      = false;
  cfg.spin_period_ns = 100'000;

  RTExecutor exec(cfg);

  for (int i = 0; i < 50; ++i) {
    exec.enqueue([] { /* intentionally empty */ });
  }

  auto th = run_async(exec, std::chrono::milliseconds(300));
  th.join();

  const JitterStats stats = exec.compute_stats();
  EXPECT_GT(stats.total_callbacks, 0u);
  EXPECT_GE(stats.p99_ns, stats.p50_ns);
  EXPECT_GE(stats.p50_ns, stats.min_ns);
  EXPECT_LE(stats.p99_ns, stats.max_ns);
}

// ── spin_once without spin ────────────────────────────────────────────────────

TEST(RTExecutor, SpinOnceDispatchesSingleCallback) {
  RTExecutorConfig cfg;
  cfg.use_sched_fifo = false;

  RTExecutor exec(cfg);

  std::atomic<int> count{0};
  exec.enqueue([&count] { count.fetch_add(1); });
  exec.enqueue([&count] { count.fetch_add(1); });

  // Only one dispatch.
  const bool dispatched = exec.spin_once();
  EXPECT_TRUE(dispatched);
  EXPECT_EQ(count.load(), 1);

  // Second dispatch.
  exec.spin_once();
  EXPECT_EQ(count.load(), 2);

  // Third call on empty queue returns false.
  EXPECT_FALSE(exec.spin_once());
}
