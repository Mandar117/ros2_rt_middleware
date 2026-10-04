#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <rclcpp/rclcpp.hpp>

#include "csv_logger.hpp"
#include "latency_publisher.hpp"
#include "latency_subscriber.hpp"
#include "rt_executor.hpp"

namespace fs = std::filesystem;
using namespace rt_middleware;
using namespace std::chrono_literals;

// ── helpers ──────────────────────────────────────────────────────────────────

struct CsvRow {
  int64_t  timestamp_ns;
  uint32_t size;
  int      transport;
  uint32_t rate;
  int64_t  latency_ns;
  int      type;
  uint64_t seq;
};

static std::vector<CsvRow> read_rows(const std::string& path, std::string* header = nullptr) {
  std::vector<CsvRow> rows;
  std::ifstream f(path);
  std::string line;
  std::getline(f, line);
  if (header) *header = line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    std::istringstream ss(line);
    std::string c[7];
    for (auto& field : c) std::getline(ss, field, ',');
    rows.push_back({std::stoll(c[0]), static_cast<uint32_t>(std::stoul(c[1])),
                    std::stoi(c[2]), static_cast<uint32_t>(std::stoul(c[3])),
                    std::stoll(c[4]), std::stoi(c[5]), std::stoull(c[6])});
  }
  return rows;
}

// Spin pub + sub on one stock executor for `duration`, then cancel.
static void spin_pair(std::shared_ptr<LatencyPublisher> pub,
                      std::shared_ptr<LatencySubscriber> sub,
                      std::chrono::milliseconds duration) {
  rclcpp::executors::SingleThreadedExecutor exec;
  exec.add_node(pub);
  exec.add_node(sub);
  std::thread t([&exec] { exec.spin(); });
  std::this_thread::sleep_for(duration);
  exec.cancel();
  t.join();
}

// ── fixture ───────────────────────────────────────────────────────────────────

class LatencyEndToEnd : public ::testing::Test {
protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    const std::string id = std::string(info->name()) + "_" + std::to_string(getpid());
    csv_path_ = (fs::temp_directory_path() / ("rt_e2e_" + id + ".csv")).string();
    shm_name_ = "/rt_e2e_" + id;
    // Unique topics so concurrently running tests never cross-talk.
    topic_joint_ = "e2e/" + std::string(info->name()) + "/joint";
    topic_cloud_ = "e2e/" + std::string(info->name()) + "/cloud";
    fs::remove(csv_path_);
  }

  void TearDown() override { fs::remove(csv_path_); }

  PublisherConfig pub_cfg(PublishMode mode, uint32_t hz) const {
    PublisherConfig c;
    c.mode        = mode;
    c.rate_hz     = hz;
    c.shm_name    = shm_name_;
    c.topic_joint = topic_joint_;
    c.topic_cloud = topic_cloud_;
    return c;
  }

  SubscriberConfig sub_cfg(TransportMode mode, uint32_t hz) const {
    SubscriberConfig c;
    c.transport_mode  = mode;
    c.publish_rate_hz = hz;
    c.shm_name        = shm_name_;
    c.topic_joint     = topic_joint_;
    c.topic_cloud     = topic_cloud_;
    return c;
  }

  // Runs one publisher/subscriber pair and returns the logged rows.
  std::vector<CsvRow> run(PublishMode pm, TransportMode tm, uint32_t hz,
                          std::chrono::milliseconds duration,
                          uint64_t* received = nullptr, uint64_t* lost = nullptr) {
    auto logger = std::make_shared<CsvLogger>(csv_path_);
    auto sub    = std::make_shared<LatencySubscriber>(sub_cfg(tm, hz), logger);
    auto pub    = std::make_shared<LatencyPublisher>(pub_cfg(pm, hz));
    spin_pair(pub, sub, duration);
    if (received) *received = sub->samples_received();
    if (lost) *lost = sub->samples_lost();
    sub.reset();
    logger.reset();  // flush buffered records to disk before reading
    return read_rows(csv_path_);
  }

  static void expect_sane(const std::vector<CsvRow>& rows, int transport) {
    ASSERT_FALSE(rows.empty()) << "CSV has no data rows";
    std::set<int> types;
    for (const auto& r : rows) {
      EXPECT_GT(r.latency_ns, 0);
      EXPECT_LT(r.latency_ns, 1'000'000'000) << "latency above 1 s";
      EXPECT_EQ(r.transport, transport);
      types.insert(r.type);
      if (r.type == static_cast<int>(MessageType::Joint)) {
        EXPECT_EQ(r.size, kJointBytes);
      } else {
        EXPECT_EQ(r.size, kCloudBytes);
      }
    }
    EXPECT_EQ(types.size(), 2u) << "expected both joint and cloud samples";
  }

  std::string csv_path_;
  std::string shm_name_;
  std::string topic_joint_;
  std::string topic_cloud_;
};

// ── transports ───────────────────────────────────────────────────────────────

TEST_F(LatencyEndToEnd, CsvHeaderHasAllColumns) {
  { CsvLogger logger(csv_path_); }
  std::string header;
  read_rows(csv_path_, &header);
  EXPECT_EQ(header,
            "timestamp_ns,message_size_bytes,transport_mode,publish_rate_hz,"
            "latency_ns,message_type,seq");
}

TEST_F(LatencyEndToEnd, CopyModeReceivesBothMessageTypes) {
  uint64_t received = 0;
  const auto rows = run(PublishMode::Copy, TransportMode::Copy, 200, 1000ms, &received);
  EXPECT_GT(received, 0u);
  expect_sane(rows, static_cast<int>(TransportMode::Copy));
}

TEST_F(LatencyEndToEnd, LoanedModeReceivesSamples) {
  // Works whether or not the RMW can loan: without loans it falls back to copy.
  const auto rows = run(PublishMode::Loaned, TransportMode::Loaned, 200, 1000ms);
  expect_sane(rows, static_cast<int>(TransportMode::Loaned));
}

TEST_F(LatencyEndToEnd, ShmModeBypassesDdsAndReceivesBothTypes) {
  uint64_t received = 0;
  uint64_t lost = 0;
  const auto rows = run(PublishMode::Shm, TransportMode::Shm, 200, 1000ms, &received, &lost);
  EXPECT_GT(received, 100u);
  EXPECT_EQ(lost, 0u) << "SHM channel lost samples at 200 Hz";
  expect_sane(rows, static_cast<int>(TransportMode::Shm));
}

TEST_F(LatencyEndToEnd, SequenceNumbersIncreasePerType) {
  const auto rows = run(PublishMode::Shm, TransportMode::Shm, 200, 500ms);
  ASSERT_FALSE(rows.empty());
  uint64_t last[2] = {0, 0};
  bool seen[2] = {false, false};
  for (const auto& r : rows) {
    if (seen[r.type]) EXPECT_GT(r.seq, last[r.type]);
    seen[r.type] = true;
    last[r.type] = r.seq;
  }
}

// ── publisher drivers ────────────────────────────────────────────────────────

TEST_F(LatencyEndToEnd, StockTimerRecordsWakeupLateness) {
  auto logger = std::make_shared<CsvLogger>(csv_path_);
  auto sub    = std::make_shared<LatencySubscriber>(sub_cfg(TransportMode::Shm, 200), logger);
  auto pub    = std::make_shared<LatencyPublisher>(pub_cfg(PublishMode::Shm, 200));
  spin_pair(pub, sub, 500ms);

  const JitterStats w = pub->timer_wakeup_stats();
  EXPECT_EQ(w.total_callbacks, pub->published());
  EXPECT_GT(w.total_callbacks, 50u);
  EXPECT_LE(w.p50_ns, w.p99_ns);
}

TEST_F(LatencyEndToEnd, RTExecutorDrivesExternallyTimedPublisher) {
  auto cfg = pub_cfg(PublishMode::Shm, 200);
  cfg.internal_timer = false;

  auto logger = std::make_shared<CsvLogger>(csv_path_);
  auto sub    = std::make_shared<LatencySubscriber>(sub_cfg(TransportMode::Shm, 200), logger);
  auto pub    = std::make_shared<LatencyPublisher>(cfg);

  RTExecutorConfig rt_cfg;
  rt_cfg.sched_policy  = SchedPolicy::Other;
  rt_cfg.busy_wait     = false;
  rt_cfg.idle_sleep_ns = 1'000'000'000LL;
  RTExecutor exec(rt_cfg);
  exec.set_periodic_task(5ms, [&pub] { pub->publish_once(); });

  std::thread t([&exec] { exec.spin(); });
  std::this_thread::sleep_for(500ms);
  exec.stop();
  t.join();
  std::this_thread::sleep_for(50ms);  // let the reader drain

  const JitterStats w = exec.wakeup_stats();
  EXPECT_EQ(w.total_callbacks, pub->published());
  EXPECT_GE(pub->published(), 50u);
  EXPECT_LE(pub->published(), 101u);
  EXPECT_EQ(pub->timer_wakeup_stats().total_callbacks, 0u) << "internal timer should be off";
  EXPECT_GT(sub->samples_received(), 0u);
}

// ── rclcpp init / shutdown ────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  rclcpp::init(argc, argv);
  const int rc = RUN_ALL_TESTS();
  rclcpp::shutdown();
  return rc;
}
