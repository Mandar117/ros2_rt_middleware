#pragma once

// Helpers shared by benchmark_runner and the standalone latency nodes:
// argument parsing, mode names, a CPU/cache load generator, and a snapshot of
// the environment the numbers were measured on.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <time.h>
#include <sys/resource.h>
#include <sys/utsname.h>

#include <rclcpp/rclcpp.hpp>
#include <rmw/rmw.h>

#include "csv_logger.hpp"
#include "latency_publisher.hpp"
#include "rt_executor.hpp"

namespace rt_middleware::bench {

// ── Arguments ────────────────────────────────────────────────────────────────

// Minimal "--key value" / "--flag" parser. ROS arguments after --ros-args are
// ignored (rclcpp::init consumes them).
class Args {
public:
  Args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      const std::string a(argv[i]);
      if (a == "--ros-args") break;
      if (a.rfind("--", 0) != 0) continue;
      const bool has_value = i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0;
      kv_.emplace_back(a.substr(2), has_value ? argv[++i] : "");
    }
  }

  bool has(const std::string& key) const {
    for (const auto& [k, v] : kv_) if (k == key) return true;
    return false;
  }

  std::string get(const std::string& key, const std::string& def) const {
    for (const auto& [k, v] : kv_) if (k == key) return v;
    return def;
  }

  long get_int(const std::string& key, long def) const {
    const std::string v = get(key, "");
    return v.empty() ? def : std::stol(v);
  }

  double get_double(const std::string& key, double def) const {
    const std::string v = get(key, "");
    return v.empty() ? def : std::stod(v);
  }

  // Comma-separated list.
  std::vector<std::string> get_list(const std::string& key, const std::string& def) const {
    std::vector<std::string> out;
    std::stringstream ss(get(key, def));
    std::string item;
    while (std::getline(ss, item, ',')) {
      if (!item.empty()) out.push_back(item);
    }
    return out;
  }

private:
  std::vector<std::pair<std::string, std::string>> kv_;
};

// ── Names ────────────────────────────────────────────────────────────────────

inline PublishMode parse_mode(const std::string& s) {
  if (s == "copy")   return PublishMode::Copy;
  if (s == "loaned") return PublishMode::Loaned;
  if (s == "shm")    return PublishMode::Shm;
  throw std::invalid_argument("unknown transport mode '" + s + "' (copy|loaned|shm)");
}

inline std::string mode_name(PublishMode m) {
  switch (m) {
    case PublishMode::Copy:   return "copy";
    case PublishMode::Loaned: return "loaned";
    case PublishMode::Shm:    return "shm";
  }
  return "unknown";
}

inline TransportMode to_transport(PublishMode m) {
  switch (m) {
    case PublishMode::Copy:   return TransportMode::Copy;
    case PublishMode::Loaned: return TransportMode::Loaned;
    case PublishMode::Shm:    return TransportMode::Shm;
  }
  return TransportMode::Copy;
}

inline SchedPolicy parse_sched(const std::string& s) {
  if (s == "fifo")  return SchedPolicy::Fifo;
  if (s == "rr")    return SchedPolicy::RoundRobin;
  if (s == "other") return SchedPolicy::Other;
  throw std::invalid_argument("unknown scheduling policy '" + s + "' (fifo|rr|other)");
}

// ── CPU load ─────────────────────────────────────────────────────────────────

// Background interference: each thread streams through a buffer larger than
// a typical L2 (evicting the measured threads' cache lines) and performs heap
// allocations (exercising allocator locks). This is the kind of co-located
// load an RT control loop has to survive.
class CpuLoad {
public:
  explicit CpuLoad(int threads) {
    for (int t = 0; t < threads; ++t) {
      threads_.emplace_back([this] {
        std::vector<uint64_t> buf(1 << 20);  // 8 MB
        uint64_t x = 0x9E3779B97F4A7C15ULL;
        while (!stop_.load(std::memory_order_relaxed)) {
          for (std::size_t i = 0; i < buf.size(); i += 8) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            buf[i] += x;
          }
          std::vector<char> churn(64 * 1024, static_cast<char>(x));
          sink_.fetch_add(static_cast<uint64_t>(churn[x % churn.size()]),
                          std::memory_order_relaxed);
        }
      });
    }
  }
  ~CpuLoad() {
    stop_.store(true);
    for (auto& t : threads_) t.join();
  }
  CpuLoad(const CpuLoad&) = delete;
  CpuLoad& operator=(const CpuLoad&) = delete;

private:
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> sink_{0};
  std::vector<std::thread> threads_;
};

// ── Environment ──────────────────────────────────────────────────────────────

inline std::string read_first_line(const std::string& path) {
  std::ifstream f(path);
  std::string line;
  std::getline(f, line);
  return line;
}

// Rate of CLOCK_MONOTONIC_RAW (what rcl timers schedule on) relative to
// CLOCK_MONOTONIC (what RTExecutor and all latency stamps use), in ppm,
// measured over `window`. A few ppm on bare metal; can be tens of thousands
// under virtualisation (WSL2), which makes rclcpp timers run fast or slow.
inline double clock_raw_vs_mono_ppm(std::chrono::milliseconds window = std::chrono::milliseconds(300)) {
  auto read = [](clockid_t id) {
    timespec ts{};
    clock_gettime(id, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
  };
  const int64_t m0 = read(CLOCK_MONOTONIC), r0 = read(CLOCK_MONOTONIC_RAW);
  std::this_thread::sleep_for(window);
  const int64_t m1 = read(CLOCK_MONOTONIC), r1 = read(CLOCK_MONOTONIC_RAW);
  return (static_cast<double>(r1 - r0) / static_cast<double>(m1 - m0) - 1.0) * 1e6;
}

// key=value lines describing where the numbers came from.
inline std::string environment_summary() {
  std::ostringstream o;
  utsname u{};
  uname(&u);
  o << "kernel=" << u.release << '\n';
  o << "kernel_version=" << u.version << '\n';
  o << "machine=" << u.machine << '\n';
  o << "preempt_rt=" << (read_first_line("/sys/kernel/realtime") == "1" ? "yes" : "no") << '\n';
  o << "cpus=" << std::thread::hardware_concurrency() << '\n';
  o << "rmw=" << rmw_get_implementation_identifier() << '\n';
  rlimit rl{};
  if (getrlimit(RLIMIT_RTPRIO, &rl) == 0) {
    o << "rlimit_rtprio=" << (rl.rlim_cur == RLIM_INFINITY ? -1L : static_cast<long>(rl.rlim_cur)) << '\n';
  }
  if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0) {
    o << "rlimit_memlock=" << (rl.rlim_cur == RLIM_INFINITY ? -1L : static_cast<long>(rl.rlim_cur)) << '\n';
  }
  o << "cpu_governor=" << read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor") << '\n';
  const std::string cmdline = read_first_line("/proc/cmdline");
  o << "isolcpus=" << (cmdline.find("isolcpus") != std::string::npos ? "yes" : "no") << '\n';
  o << "clock_raw_vs_mono_ppm=" << static_cast<long>(clock_raw_vs_mono_ppm()) << '\n';
  return o.str();
}

// Sleep for `seconds` (0 = forever), returning early on Ctrl-C.
inline void wait_for(double seconds) {
  const auto start = std::chrono::steady_clock::now();
  while (rclcpp::ok()) {
    if (seconds > 0 &&
        std::chrono::steady_clock::now() - start >= std::chrono::duration<double>(seconds)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

}  // namespace rt_middleware::bench
