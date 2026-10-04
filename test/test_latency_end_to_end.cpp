#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/mman.h>

#include <rclcpp/rclcpp.hpp>

#include "csv_logger.hpp"
#include "latency_publisher.hpp"
#include "latency_subscriber.hpp"

namespace fs = std::filesystem;
using namespace rt_middleware;

// ── helpers ──────────────────────────────────────────────────────────────────

// Spin pub + sub in a shared executor for `duration`, then cancel cleanly.
static uint64_t spin_pair(
    std::shared_ptr<LatencyPublisher>  pub,
    std::shared_ptr<LatencySubscriber> sub,
    std::chrono::milliseconds          duration)
{
  rclcpp::executors::SingleThreadedExecutor exec;
  exec.add_node(pub);
  exec.add_node(sub);
  std::thread t([&exec] { exec.spin(); });
  std::this_thread::sleep_for(duration);
  exec.cancel();
  t.join();
  return sub->samples_received();
}

// Count data rows in a CSV (header excluded).
static std::size_t count_csv_rows(const std::string& path)
{
  std::ifstream f(path);
  if (!f.is_open()) return 0;
  std::size_t n = 0;
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty()) ++n;
  }
  return n > 0 ? n - 1 : 0;
}

// Return true if any latency_ns column value (last field) is positive.
static bool any_positive_latency(const std::string& path)
{
  std::ifstream f(path);
  if (!f.is_open()) return false;
  std::string line;
  std::getline(f, line);  // skip header
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    const auto pos = line.rfind(',');
    if (pos != std::string::npos) {
      if (std::stoll(line.substr(pos + 1)) > 0) return true;
    }
  }
  return false;
}

// ── fixture ───────────────────────────────────────────────────────────────────

class LatencyEndToEnd : public ::testing::Test {
protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    csv_path_ = std::string("/tmp/rt_e2e_") + info->name() + ".csv";
    fs::remove(csv_path_);
  }

  void TearDown() override {
    fs::remove(csv_path_);
  }

  std::string csv_path_;
};

// ── Copy transport ────────────────────────────────────────────────────────────

TEST_F(LatencyEndToEnd, CopyModeReceivesSamples) {
  PublisherConfig pub_cfg;
  pub_cfg.mode    = PublishMode::Copy;
  pub_cfg.rate_hz = 200;

  SubscriberConfig sub_cfg;
  sub_cfg.transport_mode  = TransportMode::Copy;
  sub_cfg.publish_rate_hz = 200;

  auto logger = std::make_shared<CsvLogger>(csv_path_);
  auto pub    = std::make_shared<LatencyPublisher>(pub_cfg);
  auto sub    = std::make_shared<LatencySubscriber>(sub_cfg, logger);

  const uint64_t n = spin_pair(pub, sub, std::chrono::milliseconds(500));
  EXPECT_GT(n, 0u) << "No samples received in Copy mode";

  logger.reset();  // flush buffered records to disk before reading

  EXPECT_GE(count_csv_rows(csv_path_), 1u) << "CSV has no data rows";
  EXPECT_TRUE(any_positive_latency(csv_path_)) << "All latencies are non-positive";
}

// ── SHM transport — basic connectivity ───────────────────────────────────────

TEST_F(LatencyEndToEnd, ShmModeReceivesSamples) {
  // Clean up stale regions that a previously crashed run may have left behind.
  shm_unlink("/rt_mw_joint");
  shm_unlink("/rt_mw_cloud");

  PublisherConfig pub_cfg;
  pub_cfg.mode    = PublishMode::Shm;
  pub_cfg.rate_hz = 200;

  SubscriberConfig sub_cfg;
  sub_cfg.transport_mode  = TransportMode::Shm;
  sub_cfg.publish_rate_hz = 200;

  auto logger = std::make_shared<CsvLogger>(csv_path_);
  auto pub    = std::make_shared<LatencyPublisher>(pub_cfg);
  auto sub    = std::make_shared<LatencySubscriber>(sub_cfg, logger);

  const uint64_t n = spin_pair(pub, sub, std::chrono::milliseconds(500));
  EXPECT_GT(n, 0u) << "No samples received in SHM mode";

  logger.reset();

  EXPECT_GE(count_csv_rows(csv_path_), 1u) << "CSV has no data rows";
  EXPECT_TRUE(any_positive_latency(csv_path_)) << "All latencies are non-positive";
}

// ── SHM transport — verify payload read from SHM, not notification ────────────
//
// The SHM joint path logs message_size_bytes = kJointCount * sizeof(double)
// (96 bytes). If it fell back to the 1-element notification message instead,
// it would log 8 bytes. This test distinguishes the two paths.

TEST_F(LatencyEndToEnd, ShmModeJointSizeReflectsShmPayload) {
  shm_unlink("/rt_mw_joint");
  shm_unlink("/rt_mw_cloud");

  PublisherConfig pub_cfg;
  pub_cfg.mode    = PublishMode::Shm;
  pub_cfg.rate_hz = 100;

  SubscriberConfig sub_cfg;
  sub_cfg.transport_mode  = TransportMode::Shm;
  sub_cfg.publish_rate_hz = 100;

  auto logger = std::make_shared<CsvLogger>(csv_path_);
  auto pub    = std::make_shared<LatencyPublisher>(pub_cfg);
  auto sub    = std::make_shared<LatencySubscriber>(sub_cfg, logger);

  spin_pair(pub, sub, std::chrono::milliseconds(500));
  logger.reset();

  constexpr std::size_t kExpected = kJointCount * sizeof(double);  // 96

  std::ifstream f(csv_path_);
  std::string line;
  std::getline(f, line);  // skip header

  bool found = false;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    std::istringstream ss(line);
    std::string field;
    std::getline(ss, field, ',');  // timestamp_ns
    std::getline(ss, field, ',');  // message_size_bytes
    if (std::stoul(field) == kExpected) { found = true; break; }
  }

  EXPECT_TRUE(found)
      << "Expected a joint record with message_size_bytes=" << kExpected
      << " (full SHM payload). SHM read path may not be active.";
}

// ── rclcpp init / shutdown ────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  rclcpp::init(argc, argv);
  const int rc = RUN_ALL_TESTS();
  rclcpp::shutdown();
  return rc;
}
