// benchmark_runner.cpp
//
// Orchestrates the full benchmark matrix:
//   transport modes  × {copy, loaned, shm}
//   publish rates    × {100, 500, 1000} Hz
//   message types    × {joint_state (fixed), point_cloud (variable)}
//
// Runs as a single ROS2 node that forks publisher and subscriber execution on
// two threads. Results are written to results/latency_<mode>_<hz>Hz.csv.
//
// Usage (after colcon build):
//   ros2 run ros2_rt_middleware benchmark_runner [--duration <s>] [--core <n>]

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <rclcpp/rclcpp.hpp>

#include "csv_logger.hpp"
#include "latency_publisher.hpp"
#include "latency_subscriber.hpp"
#include "rt_executor.hpp"

namespace fs = std::filesystem;
using namespace rt_middleware;

struct BenchConfig {
  PublishMode   mode;
  uint32_t      rate_hz;
  int           duration_s{10};
  int           cpu_core{-1};
  std::string   results_dir{"results"};
};

static std::string mode_str(PublishMode m) {
  switch (m) {
    case PublishMode::Copy:   return "copy";
    case PublishMode::Loaned: return "loaned";
    case PublishMode::Shm:    return "shm";
  }
  return "unknown";
}

static void run_one(const BenchConfig& bc) {
  const std::string csv_path = bc.results_dir + "/latency_" +
                               mode_str(bc.mode) + "_" +
                               std::to_string(bc.rate_hz) + "Hz.csv";

  std::cout << "[bench] " << mode_str(bc.mode)
            << " @ " << bc.rate_hz << " Hz -> " << csv_path << "\n";

  auto logger = std::make_shared<CsvLogger>(csv_path);

  PublisherConfig pub_cfg;
  pub_cfg.mode    = bc.mode;
  pub_cfg.rate_hz = bc.rate_hz;

  SubscriberConfig sub_cfg;
  sub_cfg.publish_rate_hz  = bc.rate_hz;
  sub_cfg.transport_mode   =
      (bc.mode == PublishMode::Loaned) ? TransportMode::Loaned :
      (bc.mode == PublishMode::Shm)    ? TransportMode::Shm    :
                                         TransportMode::Copy;

  auto pub_node = std::make_shared<LatencyPublisher>(pub_cfg);
  auto sub_node = std::make_shared<LatencySubscriber>(sub_cfg, logger);

  // Real-time executor for the publisher thread.
  RTExecutorConfig rt_cfg;
  rt_cfg.cpu_core       = bc.cpu_core;
  rt_cfg.busy_wait      = true;
  rt_cfg.sched_priority = 80;

  RTExecutor exec(rt_cfg);

  std::atomic<bool> done{false};

  // RT thread: SPSC consumer — exec.spin() runs the RT dispatch loop.
  std::thread rt_thread([&] { exec.spin(); });

  // Feeder thread: SPSC producer — enqueues spin_some at 2× the publish rate
  // so ROS timers never miss a firing window between enqueue intervals.
  const auto feed_period =
      std::chrono::microseconds(500'000 / static_cast<int>(bc.rate_hz));
  std::thread pub_thread([&] {
    while (!done.load(std::memory_order_acquire)) {
      exec.enqueue([&pub_node] { rclcpp::spin_some(pub_node); });
      std::this_thread::sleep_for(feed_period);
    }
    exec.stop();
  });

  // Subscriber thread — standard spin with 200 µs yield.
  std::thread sub_thread([&] {
    while (!done.load(std::memory_order_acquire)) {
      rclcpp::spin_some(sub_node);
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  });

  std::this_thread::sleep_for(std::chrono::seconds(bc.duration_s));

  done.store(true, std::memory_order_release);

  if (pub_thread.joinable()) pub_thread.join();  // sets exec.stop() then exits
  if (rt_thread.joinable())  rt_thread.join();
  if (sub_thread.joinable()) sub_thread.join();

  const JitterStats stats = exec.compute_stats();
  std::cout << "[bench] Done. callbacks=" << stats.total_callbacks
            << " p50=" << stats.p50_ns / 1000 << " µs"
            << " p99=" << stats.p99_ns / 1000 << " µs"
            << " missed_deadlines=" << stats.missed_deadlines << "\n";
}

int main(int argc, char* argv[]) {
  rclcpp::init(argc, argv);

  int duration_s = 10;
  int cpu_core   = -1;

  for (int i = 1; i + 1 < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--duration") duration_s = std::stoi(argv[i+1]);
    if (arg == "--core")     cpu_core   = std::stoi(argv[i+1]);
  }

  fs::create_directories("results");

  // Full benchmark matrix.
  const std::vector<PublishMode> modes{
      PublishMode::Copy, PublishMode::Loaned, PublishMode::Shm};
  const std::vector<uint32_t> rates{100, 500, 1000};

  for (auto mode : modes) {
    for (auto rate : rates) {
      BenchConfig bc;
      bc.mode       = mode;
      bc.rate_hz    = rate;
      bc.duration_s = duration_s;
      bc.cpu_core   = cpu_core;
      run_one(bc);
      // Brief pause between runs to let the middleware settle.
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
  }

  rclcpp::shutdown();
  std::cout << "[bench] All runs complete. CSVs in results/\n";
  return 0;
}
