#include "latency_subscriber.hpp"

#include <chrono>

namespace rt_middleware {

namespace {

int64_t steady_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

LatencySubscriber::LatencySubscriber(const SubscriberConfig& cfg,
                                     std::shared_ptr<CsvLogger> logger)
    : Node("latency_subscriber"), cfg_(cfg), logger_(std::move(logger)) {

  if (cfg_.transport_mode == TransportMode::Shm) {
    shm_thread_ = std::thread(&LatencySubscriber::shm_loop, this);
  } else {
    rclcpp::QoS qos(rclcpp::KeepLast(10));
    qos.best_effort();

    // const& callbacks let the executor hand us loaned memory directly when
    // the RMW supports it (no copy into a shared_ptr).
    joint_sub_ = create_subscription<JointSampleMsg>(
        cfg_.topic_joint, qos,
        [this](const JointSampleMsg& msg) { on_joint(msg); });
    cloud_sub_ = create_subscription<CloudSampleMsg>(
        cfg_.topic_cloud, qos,
        [this](const CloudSampleMsg& msg) { on_cloud(msg); });
  }

  RCLCPP_INFO(get_logger(), "LatencySubscriber ready, transport=%d",
              static_cast<int>(cfg_.transport_mode));
}

LatencySubscriber::~LatencySubscriber() {
  shm_stop_.store(true, std::memory_order_release);
  if (shm_thread_.joinable()) {
    shm_thread_.join();
  }
}

bool LatencySubscriber::loans_available() const {
  return joint_sub_ && joint_sub_->can_loan_messages() &&
         cloud_sub_ && cloud_sub_->can_loan_messages();
}

void LatencySubscriber::on_joint(const JointSampleMsg& msg) {
  const int64_t receive_ns = steady_now_ns();
  record(MessageType::Joint, msg.publish_ns, msg.seq,
         static_cast<uint32_t>(kJointBytes), receive_ns);
}

void LatencySubscriber::on_cloud(const CloudSampleMsg& msg) {
  const int64_t receive_ns = steady_now_ns();
  record(MessageType::Cloud, msg.publish_ns, msg.seq,
         static_cast<uint32_t>(kCloudBytes), receive_ns);
}

void LatencySubscriber::record(MessageType type, int64_t publish_ns, uint64_t seq,
                               uint32_t bytes, int64_t receive_ns) {
  auto& next = next_seq_[static_cast<std::size_t>(type)];
  if (next != 0 && seq > next) {
    lost_.fetch_add(seq - next, std::memory_order_relaxed);
  }
  if (seq + 1 > next) {
    next = seq + 1;
  }

  logger_->log(LatencyRecord{
      .timestamp_ns       = publish_ns,
      .message_size_bytes = bytes,
      .transport_mode     = cfg_.transport_mode,
      .publish_rate_hz    = cfg_.publish_rate_hz,
      .latency_ns         = receive_ns - publish_ns,
      .message_type       = type,
      .seq                = seq,
  });
  samples_.fetch_add(1, std::memory_order_relaxed);
}

void LatencySubscriber::shm_loop() {
  ShmChannelReader reader(cfg_.shm_name, cfg_.shm_spin_ns);
  ShmSample sample;

  while (!shm_stop_.load(std::memory_order_acquire)) {
    if (!reader.is_open() && !reader.try_open()) {
      // Publisher has not created the channel yet.
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    switch (reader.read(sample, std::chrono::milliseconds(50))) {
      case ShmChannelReader::Result::Ok: {
        const int64_t receive_ns = steady_now_ns();
        const auto type = sample.type == static_cast<uint32_t>(MessageType::Cloud)
                              ? MessageType::Cloud
                              : MessageType::Joint;
        record(type, sample.publish_ns, sample.seq, sample.size, receive_ns);
        break;
      }
      case ShmChannelReader::Result::Closed:
        // Publisher went away; a new one restarts its sequence at 0.
        reader.close();
        next_seq_ = {0, 0};
        break;
      case ShmChannelReader::Result::Timeout:
      case ShmChannelReader::Result::NotOpen:
        break;
    }
  }
}

}  // namespace rt_middleware
