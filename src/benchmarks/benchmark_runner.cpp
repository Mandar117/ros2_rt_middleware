// benchmark_runner.cpp
//
// Runs the benchmark matrix in one process:
//   executors × {stock, rt}
//   transports × {copy, loaned, shm}
//   rates × {100, 500, 1000} Hz
//
// Per scenario:
//   publisher  — stock: rclcpp wall timer on a SingleThreadedExecutor
//                rt:    RTExecutor periodic task (absolute-deadline release,
//                       optional SCHED_FIFO / pinning / busy-wait / mlockall)
//   subscriber — SingleThreadedExecutor blocked in its wait set (DDS modes);
//                futex-blocked reader thread (SHM mode)
//
// Outputs in --results-dir:
//   latency_<exec>_<mode>_<rate>Hz.csv  one row per received sample
//   jitter_<exec>_<mode>_<rate>Hz.csv   publisher wake-up lateness samples
//   summary.csv                         one row per scenario
//   env.txt                             kernel / RMW / limits / arguments
//
// Usage (after colcon build):
//   ros2 run ros2_rt_middleware benchmark_runner --help

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <rclcpp/rclcpp.hpp>

#include "bench_common.hpp"
#include "csv_logger.hpp"
#include "latency_publisher.hpp"
#include "latency_subscriber.hpp"
#include "rt_executor.hpp"

namespace fs = std::filesystem;
using namespace rt_middleware;

namespace {

struct RunOptions {
  double      duration_s{10.0};
  int         load_threads{0};
  int         cpu_core{-1};
  SchedPolicy sched{SchedPolicy::Fifo};
  int         priority{80};
  bool        busy_wait{false};
  bool        lock_memory{false};
  bool        publish_cloud{true};
  int64_t     shm_spin_ns{0};
  std::string results_dir{"results"};
};

struct Scenario {
  std::string executor;  // "stock" | "rt"
  PublishMode mode;
  uint32_t    rate_hz;
};

const char* kSummaryHeader =
    "executor,mode,rate_hz,achieved_rate_hz,duration_s,load_threads,published,received,lost,"
    "logger_dropped,loans_pub,loans_sub,sched_ok,affinity_ok,mlock_ok,"
    "wake_p50_ns,wake_p99_ns,wake_p999_ns,wake_max_ns,wake_missed,wake_total\n";

void print_usage() {
  std::cout <<
R"(benchmark_runner — RT executor vs stock executor latency benchmark

  --duration <s>        seconds per scenario                (10)
  --rates <list>        publish rates in Hz                 (100,500,1000)
  --modes <list>        copy,loaned,shm                     (copy,loaned,shm)
  --executors <list>    stock,rt                            (stock,rt)
  --load <n>            background CPU/cache load threads   (0)
  --core <n>            pin the RT executor thread to core  (-1 = no pinning)
  --sched <p>           fifo | rr | other for the RT thread (fifo)
  --prio <n>            RT priority 1..99                   (80)
  --busy-wait           RT thread spins instead of sleeping between releases
  --lock-memory         mlockall() on the RT thread
  --no-cloud            publish joint samples only
  --shm-spin-ns <n>     SHM reader busy-poll budget before futex sleep (0)
  --results-dir <dir>   output directory                    (results)
)";
}

void run_one(const Scenario& sc, const RunOptions& opt, std::ofstream& summary) {
  const std::string tag = sc.executor + "_" + bench::mode_name(sc.mode) + "_" +
                          std::to_string(sc.rate_hz) + "Hz";
  const std::string latency_csv = opt.results_dir + "/latency_" + tag + ".csv";
  const std::string jitter_csv  = opt.results_dir + "/jitter_" + tag + ".csv";
  const bool rt = sc.executor == "rt";

  std::cout << "[bench] " << tag << " for " << opt.duration_s << " s" << std::endl;

  auto logger = std::make_shared<CsvLogger>(latency_csv);

  SubscriberConfig sub_cfg;
  sub_cfg.transport_mode  = bench::to_transport(sc.mode);
  sub_cfg.publish_rate_hz = sc.rate_hz;
  sub_cfg.shm_spin_ns     = opt.shm_spin_ns;
  auto sub = std::make_shared<LatencySubscriber>(sub_cfg, logger);

  PublisherConfig pub_cfg;
  pub_cfg.mode           = sc.mode;
  pub_cfg.rate_hz        = sc.rate_hz;
  pub_cfg.publish_cloud  = opt.publish_cloud;
  pub_cfg.internal_timer = !rt;
  auto pub = std::make_shared<LatencyPublisher>(pub_cfg);

  // Subscriber: blocks in its wait set until data arrives — no polling delay.
  rclcpp::executors::SingleThreadedExecutor sub_exec;
  sub_exec.add_node(sub);
  std::thread sub_thread([&sub_exec] { sub_exec.spin(); });

  // Publisher.
  RTExecutorConfig rt_cfg;
  rt_cfg.cpu_core       = opt.cpu_core;
  rt_cfg.sched_policy   = opt.sched;
  rt_cfg.sched_priority = opt.priority;
  rt_cfg.busy_wait      = opt.busy_wait;
  rt_cfg.lock_memory    = opt.lock_memory;
  rt_cfg.idle_sleep_ns  = 1'000'000'000LL;  // sleep straight to the next release
  RTExecutor rt_exec(rt_cfg);
  rclcpp::executors::SingleThreadedExecutor stock_exec;

  std::thread pub_thread;
  if (rt) {
    rt_exec.set_periodic_task(std::chrono::nanoseconds(1'000'000'000LL / sc.rate_hz),
                              [&pub] { pub->publish_once(); });
    pub_thread = std::thread([&rt_exec] { rt_exec.spin(); });
  } else {
    stock_exec.add_node(pub);
    pub_thread = std::thread([&stock_exec] { stock_exec.spin(); });
  }

  const auto run_start = std::chrono::steady_clock::now();
  bench::wait_for(opt.duration_s);

  // Stop the publisher first, give in-flight samples time to land, then stop
  // the subscriber.
  if (rt) rt_exec.stop(); else stock_exec.cancel();
  pub_thread.join();
  const double run_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - run_start).count();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  sub_exec.cancel();
  sub_thread.join();

  const JitterStats wake = rt ? rt_exec.wakeup_stats() : pub->timer_wakeup_stats();
  const bool jitter_ok = rt ? rt_exec.write_samples_csv(jitter_csv)
                            : pub->write_timer_samples_csv(jitter_csv);
  if (!jitter_ok) {
    std::cerr << "[bench] could not write " << jitter_csv << "\n";
  }

  const AppliedThreadConfig applied = rt ? rt_exec.applied_config() : AppliedThreadConfig{};
  const uint64_t published  = pub->published();
  const uint64_t received   = sub->samples_received();
  const uint64_t lost       = sub->samples_lost();
  const bool loans_pub      = pub->loans_available();
  const bool loans_sub      = sub->loans_available();

  sub.reset();                              // stops the SHM reader thread
  const uint64_t dropped = logger->dropped();
  logger.reset();                           // flushes the CSV

  const double achieved_hz = run_s > 0 ? static_cast<double>(published) / run_s : 0.0;
  summary << sc.executor << ',' << bench::mode_name(sc.mode) << ',' << sc.rate_hz << ','
          << achieved_hz << ','
          << opt.duration_s << ',' << opt.load_threads << ','
          << published << ',' << received << ',' << lost << ',' << dropped << ','
          << loans_pub << ',' << loans_sub << ','
          << applied.sched_ok << ',' << applied.affinity_ok << ',' << applied.mlock_ok << ','
          << wake.p50_ns << ',' << wake.p99_ns << ',' << wake.p999_ns << ',' << wake.max_ns << ','
          << wake.missed_deadlines << ',' << wake.total_callbacks << '\n';
  summary.flush();

  std::cout << std::fixed << std::setprecision(1)
            << "[bench]   published=" << published << " (" << achieved_hz << " Hz) received=" << received
            << " lost=" << lost << " logger_dropped=" << dropped
            << " | wake p50=" << wake.p50_ns / 1e3 << " us p99=" << wake.p99_ns / 1e3
            << " us max=" << wake.max_ns / 1e3 << " us missed=" << wake.missed_deadlines
            << (sc.mode == PublishMode::Loaned
                    ? (loans_pub ? " | loans: ACTIVE" : " | loans: unavailable (copy fallback)")
                    : "")
            << std::endl;
}

}  // namespace

int main(int argc, char* argv[]) {
  rclcpp::init(argc, argv);
  bench::Args args(argc, argv);

  if (args.has("help") || args.has("h")) {
    print_usage();
    rclcpp::shutdown();
    return 0;
  }

  RunOptions opt;
  std::vector<std::string> executors;
  std::vector<PublishMode> modes;
  std::vector<uint32_t> rates;
  try {
    opt.duration_s    = args.get_double("duration", 10.0);
    opt.load_threads  = static_cast<int>(args.get_int("load", 0));
    opt.cpu_core      = static_cast<int>(args.get_int("core", -1));
    opt.sched         = bench::parse_sched(args.get("sched", "fifo"));
    opt.priority      = static_cast<int>(args.get_int("prio", 80));
    opt.busy_wait     = args.has("busy-wait");
    opt.lock_memory   = args.has("lock-memory");
    opt.publish_cloud = !args.has("no-cloud");
    opt.shm_spin_ns   = args.get_int("shm-spin-ns", 0);
    opt.results_dir   = args.get("results-dir", "results");

    executors = args.get_list("executors", "stock,rt");
    for (const auto& e : executors) {
      if (e != "stock" && e != "rt") {
        throw std::invalid_argument("unknown executor '" + e + "' (stock|rt)");
      }
    }
    for (const auto& m : args.get_list("modes", "copy,loaned,shm")) {
      modes.push_back(bench::parse_mode(m));
    }
    for (const auto& r : args.get_list("rates", "100,500,1000")) {
      const long hz = std::stol(r);
      if (hz <= 0 || hz > 100'000) throw std::invalid_argument("rate out of range: " + r);
      rates.push_back(static_cast<uint32_t>(hz));
    }
  } catch (const std::exception& e) {
    std::cerr << "benchmark_runner: " << e.what() << "\n\n";
    print_usage();
    rclcpp::shutdown();
    return 2;
  }

  fs::create_directories(opt.results_dir);

  const std::string env_text = bench::environment_summary();
  {
    std::ofstream env(opt.results_dir + "/env.txt");
    env << env_text << "args=";
    for (int i = 1; i < argc; ++i) env << argv[i] << ' ';
    env << '\n';
  }
  std::cout << env_text;

  std::ofstream summary(opt.results_dir + "/summary.csv", std::ios::out | std::ios::trunc);
  summary << kSummaryHeader;

  std::unique_ptr<bench::CpuLoad> load;
  if (opt.load_threads > 0) {
    std::cout << "[bench] background load: " << opt.load_threads << " threads\n";
    load = std::make_unique<bench::CpuLoad>(opt.load_threads);
  }

  for (const auto& exec : executors) {
    for (auto mode : modes) {
      for (auto rate : rates) {
        if (!rclcpp::ok()) break;
        run_one({exec, mode, rate}, opt, summary);
        // Let the middleware settle between runs.
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    }
  }

  load.reset();
  rclcpp::shutdown();
  std::cout << "[bench] All runs complete. Results in " << opt.results_dir << "/\n";
  return 0;
}
