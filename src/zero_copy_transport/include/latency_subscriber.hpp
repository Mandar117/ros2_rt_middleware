#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/float64_multi_array.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>

#include "csv_logger.hpp"

namespace rt_middleware {

struct SubscriberConfig {
  TransportMode transport_mode{TransportMode::Copy};
  uint32_t      publish_rate_hz{1000};
  std::string   topic_joint{"rt/joint_state"};
  std::string   topic_cloud{"rt/point_cloud"};
  std::string   csv_output_path{"results/latency.csv"};
};

// LatencySubscriber receives messages published by LatencyPublisher, extracts
// the embedded timestamp from the header, and logs the one-way latency.
//
// Measurement methodology:
//   publish_time  — steady_clock nanoseconds packed into header by publisher
//   receive_time  — steady_clock::now() at the first line of the callback
//   latency       — receive_time - publish_time
//
// Caveat: both nodes must share the same clock domain (i.e., run on the same
// host). For cross-host scenarios, replace with a PTP-synced hardware clock.
class LatencySubscriber : public rclcpp::Node {
public:
  explicit LatencySubscriber(const SubscriberConfig& cfg,
                              std::shared_ptr<CsvLogger> logger);

  uint64_t samples_received() const noexcept {
    return samples_.load(std::memory_order_relaxed);
  }

private:
  void on_joint_state(
      const std_msgs::msg::Float64MultiArray::ConstSharedPtr& msg);

  void on_point_cloud(
      const sensor_msgs::msg::PointCloud2::ConstSharedPtr& msg);

  // Extract the publisher's steady_clock nanoseconds from msg header.
  static int64_t extract_publish_ns(const std_msgs::msg::Header& hdr);

  SubscriberConfig cfg_;
  std::shared_ptr<CsvLogger> logger_;

  rclcpp::Subscription<std_msgs::msg::Float64MultiArray>::SharedPtr joint_sub_;
  rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr    cloud_sub_;

  std::atomic<uint64_t> samples_{0};
};

}  // namespace rt_middleware
