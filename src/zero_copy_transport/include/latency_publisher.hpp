#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <rclcpp/rclcpp.hpp>

#include "ros2_rt_middleware/msg/cloud_sample.hpp"
#include "ros2_rt_middleware/msg/joint_sample.hpp"

#include "csv_logger.hpp"
#include "latency_stats.hpp"
#include "shm_channel.hpp"

namespace rt_middleware {

using JointSampleMsg = ros2_rt_middleware::msg::JointSample;
using CloudSampleMsg = ros2_rt_middleware::msg::CloudSample;

inline constexpr std::size_t kJointCount      = 12;
inline constexpr std::size_t kCloudPoints     = 320;
inline constexpr std::size_t kCloudPointStep  = 3 * sizeof(float);
inline constexpr std::size_t kJointBytes      = kJointCount * sizeof(double);      // 96
inline constexpr std::size_t kCloudBytes      = kCloudPoints * kCloudPointStep;    // 3840

static_assert(std::tuple_size_v<decltype(JointSampleMsg::positions)> == kJointCount,
              "JointSample.msg and kJointCount disagree");
static_assert(std::tuple_size_v<decltype(CloudSampleMsg::data)> == kCloudBytes,
              "CloudSample.msg and kCloudBytes disagree");

// Transport mode for a publisher session.
enum class PublishMode {
  Copy,    // standard publish(const&) — serialised and copied by the RMW
  Loaned,  // borrow_loaned_message — zero-copy when the RMW can loan this type
  Shm,     // ShmChannel — POSIX shared memory + futex, bypasses DDS entirely
};

struct PublisherConfig {
  PublishMode mode{PublishMode::Copy};
  uint32_t    rate_hz{1000};
  std::string topic_joint{"rt/joint_sample"};
  std::string topic_cloud{"rt/cloud_sample"};
  std::string shm_name{kShmDefaultName};
  bool        publish_cloud{true};
  // true:  an rclcpp wall timer drives publishing on whatever executor spins
  //        this node (the stock-executor baseline). Its wake-up lateness is
  //        recorded in timer_wakeup_stats().
  // false: no timer; the caller drives publish_once(), e.g. from an
  //        RTExecutor periodic task.
  bool        internal_timer{true};
  std::size_t max_samples{65'536};
};

// LatencyPublisher publishes JointSample and CloudSample messages stamped with
// the publisher's steady_clock time. The subscriber computes one-way latency
// from that stamp.
class LatencyPublisher : public rclcpp::Node {
public:
  explicit LatencyPublisher(const PublisherConfig& cfg);
  ~LatencyPublisher() override;

  // Publish one joint sample (and one cloud sample if enabled). Call from one
  // thread at a time.
  void publish_once();

  uint64_t published() const noexcept { return seq_.load(std::memory_order_relaxed); }

  // True when the RMW actually loans memory for these types (real zero-copy).
  // If false, Loaned mode falls back to an ordinary copy publish.
  bool loans_available() const;

  // Wake-up lateness of the internal wall timer (internal_timer=true only).
  // Call after the executor spinning this node has stopped.
  JitterStats timer_wakeup_stats() const { return timer_samples_.compute(timer_missed_); }
  bool write_timer_samples_csv(const std::string& path) const;

private:
  void on_timer();
  void fill_joint(JointSampleMsg& m, int64_t now_ns, uint64_t seq);
  void fill_cloud(CloudSampleMsg& m, int64_t now_ns, uint64_t seq);

  PublisherConfig cfg_;
  std::chrono::nanoseconds period_;

  rclcpp::Publisher<JointSampleMsg>::SharedPtr joint_pub_;
  rclcpp::Publisher<CloudSampleMsg>::SharedPtr cloud_pub_;
  rclcpp::TimerBase::SharedPtr timer_;
  std::unique_ptr<ShmChannelWriter> shm_;

  // Payload sources, generated once. Every mode copies them into its outgoing
  // buffer exactly once per publish, so producer-side work is identical.
  std::array<double, kJointCount>  joint_payload_{};
  std::array<uint8_t, kCloudBytes> cloud_payload_{};

  // Pre-allocated outgoing messages for Copy mode.
  JointSampleMsg joint_msg_;
  CloudSampleMsg cloud_msg_;

  std::atomic<uint64_t> seq_{0};
  bool loan_fallback_warned_{false};

  // Timer-mode wake-up accounting (same rules as RTExecutor periodic task).
  // Measured on rcl's own steady clock: rcl schedules timers on
  // CLOCK_MONOTONIC_RAW, which can run at a different rate from
  // CLOCK_MONOTONIC (by several percent under WSL2). Using the timer's own
  // clock isolates scheduling jitter from that rate error.
  rclcpp::Clock timer_clock_{RCL_STEADY_TIME};
  int64_t   next_expected_ns_{0};
  SampleSet timer_samples_;
  uint64_t  timer_missed_{0};
};

}  // namespace rt_middleware
