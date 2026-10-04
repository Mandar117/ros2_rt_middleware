#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include <rclcpp/rclcpp.hpp>

#include "csv_logger.hpp"
#include "latency_publisher.hpp"
#include "shm_channel.hpp"

namespace rt_middleware {

struct SubscriberConfig {
  TransportMode transport_mode{TransportMode::Copy};
  uint32_t      publish_rate_hz{1000};
  std::string   topic_joint{"rt/joint_sample"};
  std::string   topic_cloud{"rt/cloud_sample"};
  std::string   shm_name{kShmDefaultName};
  int64_t       shm_spin_ns{0};   // SHM reader busy-poll budget before futex sleep
};

// LatencySubscriber receives samples from LatencyPublisher and logs one-way
// latency for each one.
//
// Measurement methodology:
//   publish_time  — steady_clock nanoseconds stamped by the publisher
//   receive_time  — steady_clock::now() as soon as the sample is in hand
//                   (first line of the DDS callback; right after the SHM read)
//   latency       — receive_time - publish_time
//
// Copy / Loaned modes use ROS subscriptions; spin this node on an executor.
// Shm mode reads the ShmChannel on an internal thread and creates no ROS
// subscriptions, so spinning the node is optional.
//
// Caveat: both ends must share the same clock domain (same host).
class LatencySubscriber : public rclcpp::Node {
public:
  explicit LatencySubscriber(const SubscriberConfig& cfg,
                             std::shared_ptr<CsvLogger> logger);
  ~LatencySubscriber() override;

  uint64_t samples_received() const noexcept {
    return samples_.load(std::memory_order_relaxed);
  }
  // Sequence-number gaps after the first sample of each type, plus SHM
  // overruns. Includes samples the transport dropped.
  uint64_t samples_lost() const noexcept {
    return lost_.load(std::memory_order_relaxed);
  }
  // True when the RMW loans received messages (Copy/Loaned modes).
  bool loans_available() const;

private:
  void on_joint(const JointSampleMsg& msg);
  void on_cloud(const CloudSampleMsg& msg);
  void record(MessageType type, int64_t publish_ns, uint64_t seq,
              uint32_t bytes, int64_t receive_ns);
  void shm_loop();

  SubscriberConfig cfg_;
  std::shared_ptr<CsvLogger> logger_;

  rclcpp::Subscription<JointSampleMsg>::SharedPtr joint_sub_;
  rclcpp::Subscription<CloudSampleMsg>::SharedPtr cloud_sub_;

  std::atomic<uint64_t> samples_{0};
  std::atomic<uint64_t> lost_{0};

  // Next expected seq per MessageType; 0 = nothing seen yet. Touched only by
  // the single receiving thread.
  std::array<uint64_t, 2> next_seq_{0, 0};

  std::thread       shm_thread_;
  std::atomic<bool> shm_stop_{false};
};

}  // namespace rt_middleware
