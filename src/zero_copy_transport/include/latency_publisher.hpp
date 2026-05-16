#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/float64_multi_array.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>

#include "csv_logger.hpp"

namespace rt_middleware {

// Joint state message: 12 doubles (96 bytes) — simulates a robot arm.
static constexpr std::size_t kJointCount = 12;

// Transport mode for this publisher session.
enum class PublishMode {
  Copy,    // Standard rclcpp publish — data is copied through the middleware
  Loaned,  // Loaned message API — zero-copy when rmw supports it
  Shm,     // POSIX shared memory fallback via shm_open + mmap
};

struct PublisherConfig {
  PublishMode mode{PublishMode::Copy};
  uint32_t    rate_hz{1000};
  std::string topic_joint{"rt/joint_state"};
  std::string topic_cloud{"rt/point_cloud"};
  std::string csv_output_path{"results/latency.csv"};
  int         cloud_width{320};   // points per cloud (variable-size message)
  int         cloud_height{1};
};

// LatencyPublisher publishes joint-state (Float64MultiArray) and optionally
// point-cloud (PointCloud2) messages with a steady_clock timestamp embedded
// in each header. The subscriber computes one-way latency from that stamp.
//
// The timestamp is written as nanoseconds-since-epoch into
// header.stamp.sec / header.stamp.nanosec so it survives serialisation on
// all transports including the SHM fallback.
class LatencyPublisher : public rclcpp::Node {
public:
  explicit LatencyPublisher(const PublisherConfig& cfg);

private:
  void publish_joint_state();
  void publish_point_cloud();

  void stamp_now(std_msgs::msg::Header& hdr) const;

  PublisherConfig cfg_;

  rclcpp::Publisher<std_msgs::msg::Float64MultiArray>::SharedPtr joint_pub_;
  rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr    cloud_pub_;

  rclcpp::TimerBase::SharedPtr timer_;

  // Pre-allocated message buffers — filled once, updated in-place each cycle.
  std_msgs::msg::Float64MultiArray joint_msg_;
  sensor_msgs::msg::PointCloud2    cloud_msg_;

  uint64_t seq_{0};
};

}  // namespace rt_middleware
