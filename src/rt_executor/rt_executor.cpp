#include "rt_executor.hpp"

#include <algorithm>
#include <numeric>
#include <pthread.h>
#include <sched.h>
#include <stdexcept>
#include <thread>

#include <rclcpp/rclcpp.hpp>

namespace rt_middleware {

RTExecutor::RTExecutor(RTExecutorConfig config)
    : config_(config) {}

RTExecutor::~RTExecutor() {
  stop();
}

bool RTExecutor::enqueue(CallbackFn fn) {
  // Allocate a wrapper from the pre-warmed pool — no heap call.
  CallbackWrapper* wrapper = pool_.allocate();
  if (!wrapper) {
    return false;  // pool exhausted — caller should back-pressure
  }

  // Placement-new to construct the wrapper in pool storage.
  new (wrapper) CallbackWrapper{std::move(fn),
                                std::chrono::steady_clock::now()};

  if (!queue_.push(wrapper)) {
    // Queue full — destroy and return the slot.
    wrapper->~CallbackWrapper();
    pool_.deallocate(wrapper);
    return false;
  }
  return true;
}

bool RTExecutor::spin_once() {
  CallbackWrapper* wrapper = nullptr;
  if (!queue_.pop(wrapper) || !wrapper) {
    return false;
  }

  const auto dispatch_time = std::chrono::steady_clock::now();
  const int64_t jitter_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          dispatch_time - wrapper->enqueue_time)
          .count();

  record_jitter(jitter_ns);

  if (jitter_ns > config_.deadline_ns) {
    ++missed_deadlines_;
  }

  // Execute the callback.
  wrapper->fn();

  // Explicit destruction before returning to pool.
  wrapper->~CallbackWrapper();
  pool_.deallocate(wrapper);
  return true;
}

void RTExecutor::spin() {
  running_.store(true, std::memory_order_release);

  // Apply real-time configuration to the calling thread.
  configure_thread_affinity();
  configure_rt_scheduling();

  while (running_.load(std::memory_order_acquire)) {
    if (!spin_once()) {
      // Nothing queued — yield or spin based on config.
      if (config_.busy_wait) {
        // Busy-wait: keep the CPU and minimise wake-up latency.
        // std::atomic_thread_fence ensures the loop body isn't optimised away.
        std::atomic_thread_fence(std::memory_order_seq_cst);
      } else {
        // Sleep-based: trade latency for CPU utilisation.
        struct timespec ts{};
        ts.tv_sec = 0;
        ts.tv_nsec = config_.spin_period_ns;
        nanosleep(&ts, nullptr);
      }
    }
  }
}

void RTExecutor::stop() {
  running_.store(false, std::memory_order_release);
}

bool RTExecutor::configure_thread_affinity() {
  if (config_.cpu_core < 0) {
    return true;  // no pinning requested
  }

  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(config_.cpu_core, &cpuset);

  const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (rc != 0) {
    RCLCPP_WARN(rclcpp::get_logger("RTExecutor"),
                "pthread_setaffinity_np failed (errno %d) — "
                "is the core number valid?", rc);
    return false;
  }
  RCLCPP_INFO(rclcpp::get_logger("RTExecutor"),
              "Thread pinned to CPU core %d", config_.cpu_core);
  return true;
}

bool RTExecutor::configure_rt_scheduling() {
  if (!config_.use_sched_fifo) {
    return true;
  }

  const int policy = config_.use_sched_fifo ? SCHED_FIFO : SCHED_RR;
  struct sched_param param{};
  param.sched_priority = config_.sched_priority;

  const int rc = pthread_setschedparam(pthread_self(), policy, &param);
  if (rc != 0) {
    // CAP_SYS_NICE or a real-time group is required; warn but continue.
    RCLCPP_WARN(rclcpp::get_logger("RTExecutor"),
                "pthread_setschedparam failed (errno %d) — "
                "run with sudo or set CAP_SYS_NICE", rc);
    return false;
  }

  const char* policy_name = config_.use_sched_fifo ? "SCHED_FIFO" : "SCHED_RR";
  RCLCPP_INFO(rclcpp::get_logger("RTExecutor"),
              "Scheduling policy set to %s priority %d",
              policy_name, config_.sched_priority);
  return true;
}

void RTExecutor::record_jitter(int64_t latency_ns) {
  if (sample_count_ < kMaxSamples) {
    jitter_samples_[sample_count_++] = latency_ns;
  }
}

JitterStats RTExecutor::compute_stats() const {
  if (sample_count_ == 0) {
    return {};
  }

  // Work on a sorted copy — don't disturb the original array.
  std::vector<int64_t> sorted(jitter_samples_.begin(),
                              jitter_samples_.begin() +
                                  static_cast<ptrdiff_t>(sample_count_));
  std::sort(sorted.begin(), sorted.end());

  JitterStats stats;
  stats.total_callbacks  = sample_count_;
  stats.missed_deadlines = missed_deadlines_;
  stats.min_ns           = sorted.front();
  stats.max_ns           = sorted.back();
  stats.p50_ns           = sorted[sorted.size() / 2];
  stats.p99_ns           = sorted[static_cast<std::size_t>(
                               sorted.size() * 99 / 100)];
  return stats;
}

}  // namespace rt_middleware
