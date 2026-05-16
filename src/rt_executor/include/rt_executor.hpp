#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>

#include "lock_free_ring_buffer.hpp"
#include "memory_pool.hpp"

namespace rt_middleware {

struct RTExecutorConfig {
  int cpu_core{-1};                      // CPU core to pin to; -1 = no pinning
  int sched_priority{80};                // SCHED_FIFO priority [1–99]
  bool use_sched_fifo{true};             // false falls back to SCHED_RR
  bool busy_wait{true};                  // true = spin; false = sched_yield
  int64_t spin_period_ns{1'000'000LL};   // sleep duration when busy_wait=false
  int64_t deadline_ns{10'000'000LL};     // missed-deadline threshold (10 ms)
};

struct JitterStats {
  int64_t min_ns{0};
  int64_t max_ns{0};
  int64_t p50_ns{0};
  int64_t p99_ns{0};
  uint64_t total_callbacks{0};
  uint64_t missed_deadlines{0};
};

// RTExecutor — custom single-threaded real-time executor.
//
// Drop-in replacement for rclcpp::SingleThreadedExecutor that adds:
//   - CPU affinity via pthread_setaffinity_np
//   - SCHED_FIFO / SCHED_RR real-time scheduling
//   - Lock-free SPSC callback queue (no mutex in the hot path)
//   - Pre-allocated callback wrappers via MemoryPool (no heap in spin loop)
//   - Per-callback scheduling jitter measurement
//
// Usage:
//   RTExecutorConfig cfg;
//   cfg.cpu_core = 3;
//   cfg.sched_priority = 80;
//   RTExecutor exec(cfg);
//   exec.enqueue([&]{ node->spin_some(); });
//   exec.spin();   // blocks; call stop() from another thread or callback
class RTExecutor {
public:
  using CallbackFn = std::function<void()>;

  static constexpr std::size_t kQueueCapacity = 512;   // ring buffer slots
  static constexpr std::size_t kPoolSize = 512;         // pre-allocated wrappers
  static constexpr std::size_t kMaxSamples = 8192;      // jitter histogram size

  explicit RTExecutor(RTExecutorConfig config = {});
  ~RTExecutor();

  // Enqueue a callback. Safe to call from any thread, including the producer.
  // Returns false if the queue is full (callback is dropped).
  bool enqueue(CallbackFn fn);

  // Block until stop() is called, dispatching callbacks as they arrive.
  void spin();

  // Dispatch at most one pending callback. Returns true if one was dispatched.
  bool spin_once();

  // Signal the spin loop to exit.
  void stop();

  // Compute jitter statistics from recorded samples. Call after spin() returns.
  JitterStats compute_stats() const;

private:
  struct CallbackWrapper {
    CallbackFn fn;
    std::chrono::steady_clock::time_point enqueue_time;
  };

  bool configure_thread_affinity();
  bool configure_rt_scheduling();
  void record_jitter(int64_t latency_ns);

  RTExecutorConfig config_;
  std::atomic<bool> running_{false};

  LockFreeRingBuffer<CallbackWrapper*, kQueueCapacity> queue_;
  MemoryPool<CallbackWrapper, kPoolSize> pool_;

  // Written only from the executor thread — no atomic needed.
  std::array<int64_t, kMaxSamples> jitter_samples_{};
  std::size_t sample_count_{0};
  uint64_t missed_deadlines_{0};
};

}  // namespace rt_middleware
