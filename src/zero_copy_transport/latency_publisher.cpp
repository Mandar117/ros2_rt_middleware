#include "latency_publisher.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace rt_middleware {

namespace {

using Clock = std::chrono::steady_clock;

int64_t steady_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch())
      .count();
}

}  // namespace

LatencyPublisher::LatencyPublisher(const PublisherConfig& cfg)
    : Node("latency_publisher"),
      cfg_(cfg),
      period_(cfg.rate_hz > 0 ? 1'000'000'000LL / cfg.rate_hz : 0),
      timer_samples_(cfg.internal_timer ? cfg.max_samples : 0) {
  if (cfg_.rate_hz == 0) {
    throw std::invalid_argument("LatencyPublisher: rate_hz must be > 0");
  }

  // Deterministic payloads, generated once.
  for (std::size_t i = 0; i < kJointCount; ++i) {
    joint_payload_[i] = 0.1 * static_cast<double>(i);
  }
  for (std::size_t p = 0; p < kCloudPoints; ++p) {
    const float xyz[3] = {0.01f * static_cast<float>(p), 1.0f, -0.5f};
    std::memcpy(&cloud_payload_[p * kCloudPointStep], xyz, sizeof(xyz));
  }
  cloud_msg_.width      = static_cast<uint32_t>(kCloudPoints);
  cloud_msg_.point_step = static_cast<uint32_t>(kCloudPointStep);

  if (cfg_.mode == PublishMode::Shm) {
    shm_ = std::make_unique<ShmChannelWriter>(cfg_.shm_name);
    RCLCPP_INFO(get_logger(), "SHM channel created: %s", cfg_.shm_name.c_str());
  } else {
    rclcpp::QoS qos(rclcpp::KeepLast(10));
    qos.best_effort();
    joint_pub_ = create_publisher<JointSampleMsg>(cfg_.topic_joint, qos);
    if (cfg_.publish_cloud) {
      cloud_pub_ = create_publisher<CloudSampleMsg>(cfg_.topic_cloud, qos);
    }
    if (cfg_.mode == PublishMode::Loaned) {
      if (loans_available()) {
        RCLCPP_INFO(get_logger(), "RMW supports loaned messages — zero-copy publish active");
      } else {
        RCLCPP_WARN(get_logger(),
                    "RMW cannot loan these message types — Loaned mode falls back "
                    "to copy publish (numbers will match Copy mode)");
      }
    }
  }

  if (cfg_.internal_timer) {
    timer_ = create_wall_timer(period_, [this] { on_timer(); });
    // The timer's first release, read back from rcl so lateness is measured
    // against rcl's own schedule.
    next_expected_ns_ = timer_clock_.now().nanoseconds() + timer_->time_until_trigger().count();
  }

  RCLCPP_INFO(get_logger(), "LatencyPublisher ready: %u Hz, mode=%d, %s",
              cfg_.rate_hz, static_cast<int>(cfg_.mode),
              cfg_.internal_timer ? "internal wall timer" : "externally driven");
}

LatencyPublisher::~LatencyPublisher() {
  if (timer_) {
    timer_->cancel();
  }
}

bool LatencyPublisher::loans_available() const {
  if (!joint_pub_) {
    return false;
  }
  return joint_pub_->can_loan_messages() &&
         (!cloud_pub_ || cloud_pub_->can_loan_messages());
}

void LatencyPublisher::fill_joint(JointSampleMsg& m, int64_t now_ns, uint64_t seq) {
  m.publish_ns = now_ns;
  m.seq        = seq;
  std::copy(joint_payload_.begin(), joint_payload_.end(), m.positions.begin());
}

void LatencyPublisher::fill_cloud(CloudSampleMsg& m, int64_t now_ns, uint64_t seq) {
  m.publish_ns = now_ns;
  m.seq        = seq;
  m.width      = static_cast<uint32_t>(kCloudPoints);
  m.point_step = static_cast<uint32_t>(kCloudPointStep);
  std::memcpy(m.data.data(), cloud_payload_.data(), kCloudBytes);
}

void LatencyPublisher::publish_once() {
  const uint64_t seq = seq_.load(std::memory_order_relaxed);

  switch (cfg_.mode) {
    case PublishMode::Copy: {
      fill_joint(joint_msg_, steady_now_ns(), seq);
      joint_pub_->publish(joint_msg_);
      if (cloud_pub_) {
        fill_cloud(cloud_msg_, steady_now_ns(), seq);
        cloud_pub_->publish(cloud_msg_);
      }
      break;
    }
    case PublishMode::Loaned: {
      if (joint_pub_->can_loan_messages()) {
        auto loaned = joint_pub_->borrow_loaned_message();
        fill_joint(loaned.get(), steady_now_ns(), seq);
        joint_pub_->publish(std::move(loaned));
      } else {
        fill_joint(joint_msg_, steady_now_ns(), seq);
        joint_pub_->publish(joint_msg_);
      }
      if (cloud_pub_) {
        if (cloud_pub_->can_loan_messages()) {
          auto loaned = cloud_pub_->borrow_loaned_message();
          fill_cloud(loaned.get(), steady_now_ns(), seq);
          cloud_pub_->publish(std::move(loaned));
        } else {
          fill_cloud(cloud_msg_, steady_now_ns(), seq);
          cloud_pub_->publish(cloud_msg_);
        }
      }
      break;
    }
    case PublishMode::Shm: {
      shm_->write(static_cast<uint32_t>(MessageType::Joint), steady_now_ns(), seq,
                  joint_payload_.data(), static_cast<uint32_t>(kJointBytes));
      if (cfg_.publish_cloud) {
        shm_->write(static_cast<uint32_t>(MessageType::Cloud), steady_now_ns(), seq,
                    cloud_payload_.data(), static_cast<uint32_t>(kCloudBytes));
      }
      break;
    }
  }

  seq_.store(seq + 1, std::memory_order_relaxed);
}

void LatencyPublisher::on_timer() {
  const int64_t now = timer_clock_.now().nanoseconds();
  const int64_t period = period_.count();
  const int64_t lateness = now - next_expected_ns_;
  timer_samples_.record(lateness);
  if (lateness > period) {
    ++timer_missed_;
  }

  // Track rcl's skip rule: releases already in the past are dropped.
  next_expected_ns_ += period;
  if (now >= next_expected_ns_) {
    const int64_t skipped = (now - next_expected_ns_) / period + 1;
    next_expected_ns_ += skipped * period;
    timer_missed_ += static_cast<uint64_t>(skipped);
  }

  publish_once();
}

bool LatencyPublisher::write_timer_samples_csv(const std::string& path) const {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) {
    return false;
  }
  out << "metric,value_ns\n";
  timer_samples_.write_csv_rows(out, "wakeup");
  return static_cast<bool>(out);
}

}  // namespace rt_middleware
