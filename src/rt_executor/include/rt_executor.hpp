#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include "latency_stats.hpp"
#include "lock_free_ring_buffer.hpp"
#include "memory_pool.hpp"

namespace rt_middleware {

enum class SchedPolicy : uint8_t {
  Other,       // leave the default CFS policy untouched
  Fifo,        // SCHED_FIFO — never time-sliced against equal priority
  RoundRobin,  // SCHED_RR   — time-sliced against equal priority
};

struct RTExecutorConfig {
  int         cpu_core{-1};                    // CPU core to pin to; -1 = no pinning
  SchedPolicy sched_policy{SchedPolicy::Fifo};
  int         sched_priority{80};              // RT priority [1–99]; ignored for Other
  bool        lock_memory{false};              // mlockall(MCL_CURRENT|MCL_FUTURE) in spin()
  bool        busy_wait{true};                 // idle: true = spin; false = sleep
  int64_t     idle_sleep_ns{100'000};          // sleep slice when busy_wait=false
  int64_t     queue_deadline_ns{1'000'000};    // enqueue→dispatch above this = missed
  int64_t     wake_deadline_ns{0};             // periodic lateness above this = missed;
                                               // 0 = one full period
  std::size_t max_samples{65'536};             // per-metric sample storage (allocated
                                               // once, at construction)
};

// What spin() actually managed to apply to its thread. Useful for benchmark
// metadata: on WSL / without CAP_SYS_NICE the RT policy silently fails.
struct AppliedThreadConfig {
  bool affinity_ok{false};
  bool sched_ok{false};
  bool mlock_ok{false};
  bool timerslack_ok{false};   // PR_SET_TIMERSLACK reduced to 1 ns
};

// RTExecutor — single-threaded real-time callback executor.
//
// Two kinds of work run on the thread that calls spin():
//
//   1. A periodic task (optional, set_periodic_task()) released on absolute
//      CLOCK_MONOTONIC deadlines. Its wake-up lateness (actual start minus
//      scheduled release) is the executor's scheduling jitter.
//   2. One-shot callbacks pushed with enqueue(). Their enqueue→dispatch delay
//      is recorded separately as queue latency.
//
// Real-time properties of the spin thread:
//   - CPU affinity via pthread_setaffinity_np
//   - SCHED_FIFO / SCHED_RR via pthread_setschedparam
//   - optional mlockall to prevent page faults
//   - no heap allocation and no locks on the consumer (dispatch) path; callback
//     wrappers come from a pre-allocated MemoryPool. A std::function whose
//     captures exceed the small-buffer size (16 B in libstdc++) allocates on
//     the *producer* side and frees on the dispatch side — capture by
//     reference or pointer to keep the dispatch path allocation-free.
//
// Usage:
//   RTExecutorConfig cfg;
//   cfg.cpu_core = 3;
//   RTExecutor exec(cfg);
//   exec.set_periodic_task(1ms, [&] { control_step(); });
//   std::thread t([&] { exec.spin(); });
//   ...
//   exec.stop(); t.join();
class RTExecutor {
public:
  using CallbackFn = std::function<void()>;

  static constexpr std::size_t kQueueCapacity = 512;   // ring buffer slots
  static constexpr std::size_t kPoolSize      = 512;   // pre-allocated wrappers

  explicit RTExecutor(RTExecutorConfig config = {});
  ~RTExecutor();

  RTExecutor(const RTExecutor&) = delete;
  RTExecutor& operator=(const RTExecutor&) = delete;

  // Enqueue a one-shot callback. Safe to call from any number of threads:
  // producers serialise on a producer-side spinlock, the dispatch thread never
  // touches it. Returns false if the queue or pool is full (callback dropped).
  bool enqueue(CallbackFn fn);

  // Run `fn` every `period` on the spin thread. Must be called before spin().
  void set_periodic_task(std::chrono::nanoseconds period, CallbackFn fn);

  // Block until stop() is called. A stop() issued before spin() starts makes
  // spin() return immediately (the request is never lost).
  void spin();

  // Dispatch at most one pending one-shot callback. Returns true if one ran.
  bool spin_once();

  // Signal the spin loop to exit. Safe from any thread or from a callback.
  void stop();

  // Statistics — call after spin() has returned.
  JitterStats queue_stats() const;    // enqueue → dispatch
  JitterStats wakeup_stats() const;   // periodic release → start
  AppliedThreadConfig applied_config() const { return applied_; }

  // Write raw samples as CSV: metric,value_ns  (metric = queue | wakeup).
  bool write_samples_csv(const std::string& path) const;

private:
  struct CallbackWrapper {
    CallbackFn fn;
    std::chrono::steady_clock::time_point enqueue_time;
  };

  void apply_thread_config();
  void idle_until(std::chrono::steady_clock::time_point wake);

  RTExecutorConfig config_;
  AppliedThreadConfig applied_;
  std::atomic<bool> stop_requested_{false};

  std::atomic_flag producer_lock_ = ATOMIC_FLAG_INIT;
  LockFreeRingBuffer<CallbackWrapper*, kQueueCapacity> queue_;
  MemoryPool<CallbackWrapper, kPoolSize> pool_;

  std::chrono::nanoseconds periodic_period_{0};
  CallbackFn periodic_fn_;

  // Written only from the spin thread.
  SampleSet queue_samples_;
  SampleSet wake_samples_;
  uint64_t queue_missed_{0};
  uint64_t wake_missed_{0};
};

}  // namespace rt_middleware
