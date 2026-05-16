#include "latency_publisher.hpp"
#include "latency_subscriber.hpp"
#include "csv_logger.hpp"

#include <chrono>
#include <cstring>
#include <stdexcept>

#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/point_field.hpp>

namespace rt_middleware {

// ── helpers ──────────────────────────────────────────────────────────────────

// Pack nanoseconds-since-epoch into ROS header stamp.
// sec holds the high 32 bits, nanosec holds the low 32 bits.
static void pack_ns_into_header(std_msgs::msg::Header& hdr, int64_t ns) {
  hdr.stamp.sec     = static_cast<int32_t>(ns >> 32);
  hdr.stamp.nanosec = static_cast<uint32_t>(ns & 0xFFFF'FFFF);
}

// ── LatencyPublisher ──────────────────────────────────────────────────────────

LatencyPublisher::LatencyPublisher(const PublisherConfig& cfg)
    : Node("latency_publisher"), cfg_(cfg) {

  rclcpp::QoS qos(rclcpp::KeepLast(10));
  qos.best_effort();

  joint_pub_ = create_publisher<std_msgs::msg::Float64MultiArray>(
      cfg_.topic_joint, qos);
  cloud_pub_ = create_publisher<sensor_msgs::msg::PointCloud2>(
      cfg_.topic_cloud, qos);

  joint_msg_.layout.dim.resize(1);
  joint_msg_.layout.dim[0].label  = "joints";
  joint_msg_.layout.dim[0].size   = kJointCount;
  joint_msg_.layout.dim[0].stride = kJointCount;
  joint_msg_.data.resize(kJointCount, 0.0);

  const uint32_t point_step = 3 * sizeof(float);
  cloud_msg_.height     = static_cast<uint32_t>(cfg_.cloud_height);
  cloud_msg_.width      = static_cast<uint32_t>(cfg_.cloud_width);
  cloud_msg_.point_step = point_step;
  cloud_msg_.row_step   = static_cast<uint32_t>(cfg_.cloud_width) * point_step;
  cloud_msg_.is_dense   = true;

  auto add_field = [&](const std::string& name, uint32_t offset, uint8_t datatype) {
    sensor_msgs::msg::PointField f;
    f.name     = name;
    f.offset   = offset;
    f.datatype = datatype;
    f.count    = 1;
    cloud_msg_.fields.push_back(f);
  };
  add_field("x", 0,               sensor_msgs::msg::PointField::FLOAT32);
  add_field("y", sizeof(float),   sensor_msgs::msg::PointField::FLOAT32);
  add_field("z", 2*sizeof(float), sensor_msgs::msg::PointField::FLOAT32);

  cloud_msg_.data.resize(
      static_cast<std::size_t>(cfg_.cloud_width) *
      static_cast<std::size_t>(cfg_.cloud_height) * point_step, 0);

  const auto period = std::chrono::nanoseconds(1'000'000'000LL / cfg_.rate_hz);
  timer_ = create_wall_timer(period, [this] {
    publish_joint_state();
    publish_point_cloud();
    ++seq_;
  });

  RCLCPP_INFO(get_logger(), "LatencyPublisher ready: %u Hz, mode=%d",
              cfg_.rate_hz, static_cast<int>(cfg_.mode));
}

void LatencyPublisher::publish_joint_state() {
  const int64_t now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();

  // Embed timestamp as raw bytes in data[0] — preserves int64 without a
  // custom message type. doubles have 64-bit storage; memcpy avoids UB.
  static_assert(sizeof(double) == sizeof(int64_t));
  double ts_as_double;
  std::memcpy(&ts_as_double, &now_ns, sizeof(ts_as_double));
  joint_msg_.data[0] = ts_as_double;

  if (cfg_.mode == PublishMode::Loaned) {
    auto loaned = joint_pub_->borrow_loaned_message();
    loaned.get() = joint_msg_;
    joint_pub_->publish(std::move(loaned));
  } else {
    joint_pub_->publish(joint_msg_);
  }
}

void LatencyPublisher::publish_point_cloud() {
  const int64_t now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();

  pack_ns_into_header(cloud_msg_.header, now_ns);
  cloud_msg_.header.frame_id = "lidar";
  cloud_pub_->publish(cloud_msg_);
}

// ── LatencySubscriber ─────────────────────────────────────────────────────────

int64_t LatencySubscriber::extract_publish_ns(const std_msgs::msg::Header& hdr) {
  return (static_cast<int64_t>(hdr.stamp.sec) << 32) |
         static_cast<int64_t>(static_cast<uint32_t>(hdr.stamp.nanosec));
}

LatencySubscriber::LatencySubscriber(const SubscriberConfig& cfg,
                                     std::shared_ptr<CsvLogger> logger)
    : Node("latency_subscriber"), cfg_(cfg), logger_(std::move(logger)) {

  rclcpp::QoS qos(rclcpp::KeepLast(10));
  qos.best_effort();

  joint_sub_ = create_subscription<std_msgs::msg::Float64MultiArray>(
      cfg_.topic_joint, qos,
      [this](const std_msgs::msg::Float64MultiArray::ConstSharedPtr& msg) {
        on_joint_state(msg);
      });

  cloud_sub_ = create_subscription<sensor_msgs::msg::PointCloud2>(
      cfg_.topic_cloud, qos,
      [this](const sensor_msgs::msg::PointCloud2::ConstSharedPtr& msg) {
        on_point_cloud(msg);
      });

  RCLCPP_INFO(get_logger(), "LatencySubscriber ready, transport=%d",
              static_cast<int>(cfg_.transport_mode));
}

void LatencySubscriber::on_joint_state(
    const std_msgs::msg::Float64MultiArray::ConstSharedPtr& msg) {

  const int64_t receive_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();

  if (msg->data.empty()) return;

  int64_t publish_ns;
  std::memcpy(&publish_ns, &msg->data[0], sizeof(publish_ns));

  const int64_t latency_ns = receive_ns - publish_ns;

  LatencyRecord rec{
      .timestamp_ns       = publish_ns,
      .message_size_bytes = static_cast<uint32_t>(msg->data.size() * sizeof(double)),
      .transport_mode     = cfg_.transport_mode,
      .publish_rate_hz    = cfg_.publish_rate_hz,
      .latency_ns         = latency_ns,
  };
  logger_->log(rec);
  samples_.fetch_add(1, std::memory_order_relaxed);
}

void LatencySubscriber::on_point_cloud(
    const sensor_msgs::msg::PointCloud2::ConstSharedPtr& msg) {

  const int64_t receive_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();

  const int64_t publish_ns = extract_publish_ns(msg->header);
  const int64_t latency_ns = receive_ns - publish_ns;

  LatencyRecord rec{
      .timestamp_ns       = publish_ns,
      .message_size_bytes = msg->row_step * msg->height,
      .transport_mode     = cfg_.transport_mode,
      .publish_rate_hz    = cfg_.publish_rate_hz,
      .latency_ns         = latency_ns,
  };
  logger_->log(rec);
  samples_.fetch_add(1, std::memory_order_relaxed);
}

// ── CsvLogger ─────────────────────────────────────────────────────────────────

CsvLogger::CsvLogger(const std::string& path)
    : file_(path, std::ios::out | std::ios::trunc) {
  if (!file_.is_open()) {
    throw std::runtime_error("CsvLogger: cannot open " + path);
  }
  file_ << "timestamp_ns,message_size_bytes,transport_mode,"
           "publish_rate_hz,latency_ns\n";
  file_.flush();
  writer_thread_ = std::thread(&CsvLogger::writer_loop, this);
}

CsvLogger::~CsvLogger() {
  running_.store(false, std::memory_order_release);
  if (writer_thread_.joinable()) {
    writer_thread_.join();
  }
  LatencyRecord rec{};
  while (buffer_.pop(rec)) {
    file_ << rec.timestamp_ns << ','
          << rec.message_size_bytes << ','
          << static_cast<int>(rec.transport_mode) << ','
          << rec.publish_rate_hz << ','
          << rec.latency_ns << '\n';
  }
}

bool CsvLogger::log(const LatencyRecord& rec) noexcept {
  return buffer_.push(rec);
}

void CsvLogger::writer_loop() {
  LatencyRecord rec{};
  while (running_.load(std::memory_order_acquire)) {
    if (buffer_.pop(rec)) {
      file_ << rec.timestamp_ns << ','
            << rec.message_size_bytes << ','
            << static_cast<int>(rec.transport_mode) << ','
            << rec.publish_rate_hz << ','
            << rec.latency_ns << '\n';
    } else {
      file_.flush();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

}  // namespace rt_middleware
