#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <sys/prctl.h>

#include "rt_executor.hpp"

using rt_middleware::RTExecutor;
using rt_middleware::RTExecutorConfig;
using rt_middleware::JitterStats;
using rt_middleware::SchedPolicy;

static RTExecutorConfig test_config() {
  RTExecutorConfig cfg;
  cfg.sched_policy  = SchedPolicy::Other;  // no CAP_SYS_NICE in CI
  cfg.busy_wait     = false;
  cfg.idle_sleep_ns = 100'000;
  return cfg;
}

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
  cfg.sched_policy = rt_middleware::SchedPolicy::Other;  // no CAP_SYS_NICE in CI
  cfg.busy_wait    = false;
  cfg.idle_sleep_ns = 100'000;  // 0.1 ms

  RTExecutor exec(cfg);
  std::atomic<bool> called{false};

  EXPECT_TRUE(exec.enqueue([&called] { called.store(true); }));

  auto th = run_async(exec, std::chrono::milliseconds(200));
  th.join();

  EXPECT_TRUE(called.load());
}

TEST(RTExecutor, MultipleCallbacksAllExecute) {
  RTExecutorConfig cfg;
  cfg.sched_policy = rt_middleware::SchedPolicy::Other;
  cfg.busy_wait      = false;
  cfg.idle_sleep_ns = 100'000;

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
  cfg.sched_policy = rt_middleware::SchedPolicy::Other;
  cfg.busy_wait      = false;
  cfg.idle_sleep_ns = 100'000;

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
  cfg.sched_policy = rt_middleware::SchedPolicy::Other;
  cfg.busy_wait      = false;
  cfg.idle_sleep_ns = 1'000'000;  // 1 ms

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
  cfg.sched_policy = rt_middleware::SchedPolicy::Other;
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
  cfg.sched_policy = rt_middleware::SchedPolicy::Other;
  cfg.busy_wait      = false;
  cfg.idle_sleep_ns = 100'000;

  RTExecutor exec(cfg);

  for (int i = 0; i < 50; ++i) {
    exec.enqueue([] { /* intentionally empty */ });
  }

  auto th = run_async(exec, std::chrono::milliseconds(300));
  th.join();

  const JitterStats stats = exec.queue_stats();
  EXPECT_GT(stats.total_callbacks, 0u);
  EXPECT_GE(stats.p99_ns, stats.p50_ns);
  EXPECT_GE(stats.p50_ns, stats.min_ns);
  EXPECT_LE(stats.p99_ns, stats.max_ns);
}

// ── spin_once without spin ────────────────────────────────────────────────────

TEST(RTExecutor, SpinOnceDispatchesSingleCallback) {
  RTExecutorConfig cfg;
  cfg.sched_policy = rt_middleware::SchedPolicy::Other;

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

// ── Stop semantics ────────────────────────────────────────────────────────────

TEST(RTExecutor, StopBeforeSpinIsNotLost) {
  RTExecutor exec(test_config());
  exec.stop();  // request arrives before spin() starts

  std::atomic<bool> returned{false};
  std::thread th([&] { exec.spin(); returned.store(true); });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const bool returned_on_its_own = returned.load();
  if (!returned_on_its_own) exec.stop();  // unblock so the test can finish
  th.join();
  EXPECT_TRUE(returned_on_its_own) << "stop() issued before spin() was lost";
}

TEST(RTExecutor, CanSpinAgainAfterStop) {
  RTExecutor exec(test_config());
  std::atomic<int> count{0};

  for (int round = 0; round < 2; ++round) {
    exec.enqueue([&count] { count.fetch_add(1); });
    auto th = run_async(exec, std::chrono::milliseconds(50));
    th.join();
  }
  EXPECT_EQ(count.load(), 2);
}

// ── Multi-producer enqueue ────────────────────────────────────────────────────

TEST(RTExecutor, ConcurrentProducersAllCallbacksExecute) {
  RTExecutor exec(test_config());
  static constexpr int kProducers = 4;
  static constexpr int kPerProducer = 2'000;
  std::atomic<int> count{0};

  std::thread spinner([&exec] { exec.spin(); });

  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&exec, &count] {
      for (int i = 0; i < kPerProducer; ++i) {
        // Retry on back-pressure so every callback is eventually delivered.
        while (!exec.enqueue([&count] { count.fetch_add(1); })) {
          std::this_thread::yield();
        }
      }
    });
  }
  for (auto& t : producers) t.join();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (count.load() < kProducers * kPerProducer &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  exec.stop();
  spinner.join();

  EXPECT_EQ(count.load(), kProducers * kPerProducer);
  EXPECT_EQ(exec.queue_stats().total_callbacks,
            static_cast<uint64_t>(kProducers * kPerProducer));
}

// ── Destruction ───────────────────────────────────────────────────────────────

TEST(RTExecutor, DestructorReleasesPendingCallbacks) {
  auto token = std::make_shared<int>(42);
  {
    RTExecutor exec(test_config());
    for (int i = 0; i < 10; ++i) {
      ASSERT_TRUE(exec.enqueue([token] { (void)*token; }));
    }
    EXPECT_EQ(token.use_count(), 11);
  }  // never spun — destructor must destroy the queued callbacks
  EXPECT_EQ(token.use_count(), 1);
}

// ── Sample accounting ─────────────────────────────────────────────────────────

TEST(RTExecutor, TotalCountsAllDispatchesEvenWhenSamplesCapped) {
  RTExecutorConfig cfg = test_config();
  cfg.max_samples = 10;
  RTExecutor exec(cfg);

  for (int i = 0; i < 50; ++i) exec.enqueue([] {});
  while (exec.spin_once()) {}

  const JitterStats s = exec.queue_stats();
  EXPECT_EQ(s.total_callbacks, 50u);
  EXPECT_EQ(s.recorded_samples, 10u);
}

TEST(RTExecutor, WriteSamplesCsv) {
  RTExecutor exec(test_config());
  for (int i = 0; i < 5; ++i) exec.enqueue([] {});
  while (exec.spin_once()) {}

  const std::string path = "/tmp/rt_executor_samples_test.csv";
  ASSERT_TRUE(exec.write_samples_csv(path));

  std::ifstream in(path);
  std::string line;
  std::getline(in, line);
  EXPECT_EQ(line, "metric,value_ns");
  int rows = 0;
  while (std::getline(in, line)) {
    EXPECT_EQ(line.rfind("queue,", 0), 0u);
    ++rows;
  }
  EXPECT_EQ(rows, 5);
  std::remove(path.c_str());
}

// ── Periodic task ─────────────────────────────────────────────────────────────

TEST(RTExecutor, PeriodicTaskFiresAtConfiguredRate) {
  RTExecutor exec(test_config());
  std::atomic<int> ticks{0};
  exec.set_periodic_task(std::chrono::milliseconds(2),
                         [&ticks] { ticks.fetch_add(1); });

  auto th = run_async(exec, std::chrono::milliseconds(400));
  th.join();

  // 400 ms / 2 ms = 200 releases. Allow wide margins for loaded CI runners.
  EXPECT_GE(ticks.load(), 100);
  EXPECT_LE(ticks.load(), 201);

  const JitterStats w = exec.wakeup_stats();
  EXPECT_EQ(w.total_callbacks, static_cast<uint64_t>(ticks.load()));
  EXPECT_GE(w.min_ns, 0);
  EXPECT_LE(w.p50_ns, w.p99_ns);
}

TEST(RTExecutor, PeriodicOverrunCountsMissedDeadlines) {
  RTExecutor exec(test_config());
  // Each run takes 3 periods, so at least two releases are skipped per run.
  exec.set_periodic_task(std::chrono::milliseconds(1), [] {
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
  });

  auto th = run_async(exec, std::chrono::milliseconds(100));
  th.join();

  const JitterStats w = exec.wakeup_stats();
  EXPECT_GT(w.total_callbacks, 0u);
  EXPECT_GE(w.missed_deadlines, 2 * w.total_callbacks - 2);
}

TEST(RTExecutor, PeriodicTaskAndQueueInterleave) {
  RTExecutor exec(test_config());
  std::atomic<int> ticks{0};
  std::atomic<int> oneshots{0};
  exec.set_periodic_task(std::chrono::milliseconds(1),
                         [&ticks] { ticks.fetch_add(1); });

  std::thread spinner([&exec] { exec.spin(); });
  for (int i = 0; i < 50; ++i) {
    exec.enqueue([&oneshots] { oneshots.fetch_add(1); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  exec.stop();
  spinner.join();

  EXPECT_EQ(oneshots.load(), 50);
  EXPECT_GT(ticks.load(), 20);
}

TEST(RTExecutor, InvalidPeriodThrows) {
  RTExecutor exec(test_config());
  EXPECT_THROW(exec.set_periodic_task(std::chrono::nanoseconds(0), [] {}),
               std::invalid_argument);
}

// ── Scheduling policy ─────────────────────────────────────────────────────────

TEST(RTExecutor, RoundRobinPolicyIsApplied) {
  RTExecutorConfig cfg = test_config();
  cfg.sched_policy   = SchedPolicy::RoundRobin;
  cfg.sched_priority = 10;
  RTExecutor exec(cfg);

  std::atomic<int> observed_policy{-1};
  exec.enqueue([&observed_policy] {
    int policy = -1;
    sched_param p{};
    pthread_getschedparam(pthread_self(), &policy, &p);
    observed_policy.store(policy);
  });

  auto th = run_async(exec, std::chrono::milliseconds(50));
  th.join();

  if (!exec.applied_config().sched_ok) {
    GTEST_SKIP() << "No permission for RT scheduling (CAP_SYS_NICE / rtprio)";
  }
  EXPECT_EQ(observed_policy.load(), SCHED_RR);
}

TEST(RTExecutor, OtherPolicyLeavesThreadUntouched) {
  RTExecutor exec(test_config());
  std::atomic<int> observed_policy{-1};
  exec.enqueue([&observed_policy] {
    int policy = -1;
    sched_param p{};
    pthread_getschedparam(pthread_self(), &policy, &p);
    observed_policy.store(policy);
  });
  auto th = run_async(exec, std::chrono::milliseconds(50));
  th.join();

  EXPECT_EQ(observed_policy.load(), SCHED_OTHER);
  EXPECT_FALSE(exec.applied_config().sched_ok);
}

TEST(RTExecutor, SpinThreadRunsWithMinimalTimerSlack) {
  RTExecutor exec(test_config());
  std::atomic<long> slack{-1};
  exec.enqueue([&slack] { slack.store(prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0)); });
  auto th = run_async(exec, std::chrono::milliseconds(50));
  th.join();

  EXPECT_TRUE(exec.applied_config().timerslack_ok);
  EXPECT_EQ(slack.load(), 1) << "default 50 us slack would delay every periodic release";
}
