// latency_publisher — standalone publisher for cross-process benchmarks.
//
//   latency_publisher --rate 1000 --mode copy|loaned|shm
//                     [--executor stock|rt] [--core n] [--sched fifo|rr|other]
//                     [--prio n] [--busy-wait] [--lock-memory] [--no-cloud]
//                     [--duration s] [--jitter-out file.csv]
//
// --duration 0 (default) runs until Ctrl-C.

#include <iomanip>
#include <iostream>
#include <thread>

#include <rclcpp/rclcpp.hpp>

#include "bench_common.hpp"
#include "latency_publisher.hpp"
#include "rt_executor.hpp"

using namespace rt_middleware;

int main(int argc, char* argv[]) {
  rclcpp::init(argc, argv);
  bench::Args args(argc, argv);

  PublisherConfig cfg;
  std::string executor;
  RTExecutorConfig rt_cfg;
  double duration_s = 0.0;
  std::string jitter_out;
  try {
    cfg.rate_hz       = static_cast<uint32_t>(args.get_int("rate", 1000));
    cfg.mode          = bench::parse_mode(args.get("mode", "copy"));
    cfg.publish_cloud = !args.has("no-cloud");
    executor          = args.get("executor", "stock");
    if (executor != "stock" && executor != "rt") {
      throw std::invalid_argument("unknown executor '" + executor + "' (stock|rt)");
    }
    cfg.internal_timer = executor == "stock";

    rt_cfg.cpu_core       = static_cast<int>(args.get_int("core", -1));
    rt_cfg.sched_policy   = bench::parse_sched(args.get("sched", "fifo"));
    rt_cfg.sched_priority = static_cast<int>(args.get_int("prio", 80));
    rt_cfg.busy_wait      = args.has("busy-wait");
    rt_cfg.lock_memory    = args.has("lock-memory");
    rt_cfg.idle_sleep_ns  = 1'000'000'000LL;

    duration_s = args.get_double("duration", 0.0);
    jitter_out = args.get("jitter-out", "");
  } catch (const std::exception& e) {
    std::cerr << "latency_publisher: " << e.what() << "\n";
    rclcpp::shutdown();
    return 2;
  }

  auto pub = std::make_shared<LatencyPublisher>(cfg);
  JitterStats wake;

  if (executor == "rt") {
    RTExecutor exec(rt_cfg);
    exec.set_periodic_task(std::chrono::nanoseconds(1'000'000'000LL / cfg.rate_hz),
                           [&pub] { pub->publish_once(); });
    std::thread t([&exec] { exec.spin(); });
    bench::wait_for(duration_s);
    exec.stop();
    t.join();
    wake = exec.wakeup_stats();
    if (!jitter_out.empty()) exec.write_samples_csv(jitter_out);
  } else {
    rclcpp::executors::SingleThreadedExecutor exec;
    exec.add_node(pub);
    std::thread t([&exec] { exec.spin(); });
    bench::wait_for(duration_s);
    exec.cancel();
    t.join();
    wake = pub->timer_wakeup_stats();
    if (!jitter_out.empty()) pub->write_timer_samples_csv(jitter_out);
  }

  std::cout << std::fixed << std::setprecision(1)
            << "[publisher] published=" << pub->published()
            << " wake p50=" << wake.p50_ns / 1e3 << " us p99=" << wake.p99_ns / 1e3
            << " us max=" << wake.max_ns / 1e3 << " us missed=" << wake.missed_deadlines
            << std::endl;

  pub.reset();
  rclcpp::shutdown();
  return 0;
}
