// latency_subscriber — standalone subscriber for cross-process benchmarks.
//
//   latency_subscriber --rate 1000 --mode copy|loaned|shm
//                      --output results/latency.csv
//                      [--duration s] [--shm-spin-ns n]
//
// --duration 0 (default) runs until Ctrl-C. The CSV is flushed on exit.

#include <iostream>
#include <thread>

#include <rclcpp/rclcpp.hpp>

#include "bench_common.hpp"
#include "csv_logger.hpp"
#include "latency_subscriber.hpp"

using namespace rt_middleware;

int main(int argc, char* argv[]) {
  rclcpp::init(argc, argv);
  bench::Args args(argc, argv);

  SubscriberConfig cfg;
  std::string output;
  double duration_s = 0.0;
  try {
    cfg.publish_rate_hz = static_cast<uint32_t>(args.get_int("rate", 1000));
    cfg.transport_mode  = bench::to_transport(bench::parse_mode(args.get("mode", "copy")));
    cfg.shm_spin_ns     = args.get_int("shm-spin-ns", 0);
    output              = args.get("output", "results/latency.csv");
    duration_s          = args.get_double("duration", 0.0);
  } catch (const std::exception& e) {
    std::cerr << "latency_subscriber: " << e.what() << "\n";
    rclcpp::shutdown();
    return 2;
  }

  auto logger = std::make_shared<CsvLogger>(output);
  auto sub    = std::make_shared<LatencySubscriber>(cfg, logger);

  rclcpp::executors::SingleThreadedExecutor exec;
  exec.add_node(sub);
  std::thread t([&exec] { exec.spin(); });
  bench::wait_for(duration_s);
  exec.cancel();
  t.join();

  std::cout << "[subscriber] received=" << sub->samples_received()
            << " lost=" << sub->samples_lost()
            << " loans=" << (sub->loans_available() ? "yes" : "no") << std::endl;

  sub.reset();
  std::cout << "[subscriber] logger_dropped=" << logger->dropped()
            << " -> " << output << std::endl;
  logger.reset();
  rclcpp::shutdown();
  return 0;
}
