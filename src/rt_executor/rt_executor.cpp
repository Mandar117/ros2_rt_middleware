#include "rt_executor.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <pthread.h>
#include <sched.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <thread>
#include <time.h>

#include <rclcpp/rclcpp.hpp>

namespace rt_middleware {

namespace {

using Clock = std::chrono::steady_clock;

int64_t to_ns(Clock::duration d) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
}

// steady_clock is CLOCK_MONOTONIC on Linux, so its epoch converts directly
// to an absolute timespec for clock_nanosleep(TIMER_ABSTIME).
timespec to_timespec(Clock::time_point tp) {
  const int64_t ns = to_ns(tp.time_since_epoch());
  timespec ts{};
  ts.tv_sec  = static_cast<time_t>(ns / 1'000'000'000LL);
  ts.tv_nsec = static_cast<long>(ns % 1'000'000'000LL);
  return ts;
}

}  // namespace

RTExecutor::RTExecutor(RTExecutorConfig config)
    : config_(config),
      queue_samples_(config.max_samples),
      wake_samples_(config.max_samples) {}

RTExecutor::~RTExecutor() {
  stop();
  // Destroy callbacks that were queued but never dispatched so their captured
  // state is released. Assumes spin() is no longer running.
  CallbackWrapper* wrapper = nullptr;
  while (queue_.pop(wrapper)) {
    if (wrapper) {
      wrapper->~CallbackWrapper();
      pool_.deallocate(wrapper);
    }
  }
}

bool RTExecutor::enqueue(CallbackFn fn) {
  // Allocate a wrapper from the pre-warmed pool — no heap call.
  CallbackWrapper* wrapper = pool_.allocate();
  if (!wrapper) {
    return false;  // pool exhausted — caller should back-pressure
  }

  // Placement-new to construct the wrapper in pool storage.
  new (wrapper) CallbackWrapper{std::move(fn), Clock::now()};

  // The ring buffer is single-producer; serialise producers here so enqueue()
  // is safe from any thread. The consumer (spin thread) never takes this lock.
  while (producer_lock_.test_and_set(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  const bool pushed = queue_.push(wrapper);
  producer_lock_.clear(std::memory_order_release);

  if (!pushed) {
    // Queue full — destroy and return the slot.
    wrapper->~CallbackWrapper();
    pool_.deallocate(wrapper);
    return false;
  }
  return true;
}

void RTExecutor::set_periodic_task(std::chrono::nanoseconds period,
                                   CallbackFn fn) {
  if (period.count() <= 0) {
    throw std::invalid_argument("RTExecutor: periodic task period must be > 0");
  }
  periodic_period_ = period;
  periodic_fn_     = std::move(fn);
}

bool RTExecutor::spin_once() {
  CallbackWrapper* wrapper = nullptr;
  if (!queue_.pop(wrapper) || !wrapper) {
    return false;
  }

  const int64_t latency_ns = to_ns(Clock::now() - wrapper->enqueue_time);
  queue_samples_.record(latency_ns);
  if (latency_ns > config_.queue_deadline_ns) {
    ++queue_missed_;
  }

  // Execute the callback.
  wrapper->fn();

  // Explicit destruction before returning to pool.
  wrapper->~CallbackWrapper();
  pool_.deallocate(wrapper);
  return true;
}

void RTExecutor::idle_until(Clock::time_point wake) {
  if (config_.busy_wait) {
    // Busy-wait: keep the CPU and minimise wake-up latency. The fence keeps
    // the poll loop from being optimised into a tight register-only spin.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return;
  }
  const auto slice_end = Clock::now() + std::chrono::nanoseconds(config_.idle_sleep_ns);
  const timespec ts = to_timespec(std::min(wake, slice_end));
  // EINTR just means we re-check the loop condition early.
  clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
}

void RTExecutor::spin() {
  apply_thread_config();

  const bool periodic = periodic_fn_ && periodic_period_.count() > 0;
  const int64_t period_ns = periodic_period_.count();
  const int64_t wake_deadline_ns =
      config_.wake_deadline_ns > 0 ? config_.wake_deadline_ns : period_ns;

  auto next_release = Clock::now() + periodic_period_;

  while (!stop_requested_.load(std::memory_order_acquire)) {
    // Drain one-shot callbacks, bounded so a flooding producer cannot starve
    // the periodic task.
    std::size_t drained = 0;
    while (drained < kQueueCapacity && spin_once()) {
      ++drained;
    }

    if (periodic) {
      const auto now = Clock::now();
      if (now >= next_release) {
        const int64_t lateness = to_ns(now - next_release);
        wake_samples_.record(lateness);
        if (lateness > wake_deadline_ns) {
          ++wake_missed_;
        }

        // Same skip rule as rcl timers (rcl_timer_call): if we started so
        // late that later releases are already in the past, drop them rather
        // than firing a catch-up burst, and count each one as missed.
        next_release += periodic_period_;
        if (now >= next_release) {
          const int64_t skipped = to_ns(now - next_release) / period_ns + 1;
          next_release += std::chrono::nanoseconds(skipped * period_ns);
          wake_missed_ += static_cast<uint64_t>(skipped);
        }

        periodic_fn_();
        continue;
      }
      if (drained == 0) {
        idle_until(next_release);
      }
    } else if (drained == 0) {
      idle_until(Clock::time_point::max());
    }
  }

  // Consume the request so the executor can be spun again.
  stop_requested_.store(false, std::memory_order_release);
}

void RTExecutor::stop() {
  stop_requested_.store(true, std::memory_order_release);
}

void RTExecutor::apply_thread_config() {
  applied_ = {};

  if (config_.cpu_core >= 0) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(config_.cpu_core, &cpuset);
    const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    applied_.affinity_ok = (rc == 0);
    if (rc != 0) {
      RCLCPP_WARN(rclcpp::get_logger("RTExecutor"),
                  "pthread_setaffinity_np(core %d) failed: %s",
                  config_.cpu_core, std::strerror(rc));
    } else {
      RCLCPP_INFO(rclcpp::get_logger("RTExecutor"),
                  "Thread pinned to CPU core %d", config_.cpu_core);
    }
  }

  if (config_.sched_policy != SchedPolicy::Other) {
    const bool fifo = config_.sched_policy == SchedPolicy::Fifo;
    sched_param param{};
    param.sched_priority = config_.sched_priority;
    const int rc = pthread_setschedparam(pthread_self(),
                                         fifo ? SCHED_FIFO : SCHED_RR, &param);
    applied_.sched_ok = (rc == 0);
    if (rc != 0) {
      // CAP_SYS_NICE or an rtprio limit is required; warn but continue.
      RCLCPP_WARN(rclcpp::get_logger("RTExecutor"),
                  "pthread_setschedparam(%s, %d) failed: %s — grant CAP_SYS_NICE "
                  "or raise the rtprio limit",
                  fifo ? "SCHED_FIFO" : "SCHED_RR", config_.sched_priority,
                  std::strerror(rc));
    } else {
      RCLCPP_INFO(rclcpp::get_logger("RTExecutor"),
                  "Scheduling policy set to %s priority %d",
                  fifo ? "SCHED_FIFO" : "SCHED_RR", config_.sched_priority);
    }
  }

  // SCHED_OTHER threads get 50 µs of timer slack by default: the kernel may
  // defer every sleep expiry by up to that much to batch wake-ups. RT policies
  // get zero slack automatically, but if the RT policy could not be applied
  // (or Other was requested) we would silently inherit it. 1 ns = minimum.
  applied_.timerslack_ok = (prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL) == 0);

  if (config_.lock_memory) {
    applied_.mlock_ok = (mlockall(MCL_CURRENT | MCL_FUTURE) == 0);
    if (!applied_.mlock_ok) {
      RCLCPP_WARN(rclcpp::get_logger("RTExecutor"),
                  "mlockall failed: %s — raise the memlock limit",
                  std::strerror(errno));
    }
  }
}

JitterStats RTExecutor::queue_stats() const {
  return queue_samples_.compute(queue_missed_);
}

JitterStats RTExecutor::wakeup_stats() const {
  return wake_samples_.compute(wake_missed_);
}

bool RTExecutor::write_samples_csv(const std::string& path) const {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) {
    return false;
  }
  out << "metric,value_ns\n";
  queue_samples_.write_csv_rows(out, "queue");
  wake_samples_.write_csv_rows(out, "wakeup");
  return static_cast<bool>(out);
}

}  // namespace rt_middleware
