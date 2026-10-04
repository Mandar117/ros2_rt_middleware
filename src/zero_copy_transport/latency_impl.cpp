#include "latency_publisher.hpp"
#include "latency_subscriber.hpp"
#include "csv_logger.hpp"

#include <chrono>
#include <cstring>
#include <stdexcept>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/point_field.hpp>

namespace rt_middleware {

// ── SHM region layout ────────────────────────────────────────────────────────
//
// The joint-state SHM region holds one frame's worth of data plus the
// publisher's steady_clock timestamp. The subscriber reads this struct
// directly after receiving the lightweight notification message, avoiding
// any deserialization of the joint payload through the ROS middleware.
struct ShmJointRegion {
  int64_t  publish_ns;           // steady_clock nanoseconds at publish time
  uint32_t seq;                  // publisher sequence number
  uint32_t _pad;                 // alignment padding
  double   joints[kJointCount];  // joint positions (96 bytes)
};
static_assert(sizeof(ShmJointRegion) == 8 + 8 + kJointCount * sizeof(double),
              "ShmJointRegion layout unexpected");

// Point-cloud SHM region: fixed header followed immediately by
// width * height * point_step bytes of raw XYZ float data.
// shm_cloud_size_ = sizeof(ShmCloudHeader) + that data length.
struct ShmCloudHeader {
  int64_t  publish_ns;   // steady_clock nanoseconds at publish time
  uint32_t seq;          // publisher sequence number
  uint32_t width;        // cloud width in points
  uint32_t height;       // cloud height (1 for unorganised)
  uint32_t point_step;   // bytes per point
};
static_assert(sizeof(ShmCloudHeader) == 24, "ShmCloudHeader layout unexpected");

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

  if (cfg_.mode == PublishMode::Shm) {
    shm_joint_fd_ = shm_open(kShmJointName, O_CREAT | O_RDWR, 0666);
    if (shm_joint_fd_ < 0) {
      throw std::runtime_error("LatencyPublisher: shm_open(joint) failed");
    }
    if (ftruncate(shm_joint_fd_, static_cast<off_t>(sizeof(ShmJointRegion))) < 0) {
      throw std::runtime_error("LatencyPublisher: ftruncate(joint) failed");
    }
    shm_joint_ptr_ = mmap(nullptr, sizeof(ShmJointRegion),
                          PROT_READ | PROT_WRITE, MAP_SHARED, shm_joint_fd_, 0);
    if (shm_joint_ptr_ == MAP_FAILED) {
      shm_joint_ptr_ = nullptr;
      throw std::runtime_error("LatencyPublisher: mmap(joint) failed");
    }
    RCLCPP_INFO(get_logger(), "SHM joint region created: %s (%zu bytes)",
                kShmJointName, sizeof(ShmJointRegion));

    shm_cloud_size_ = sizeof(ShmCloudHeader) + cloud_msg_.data.size();
    shm_cloud_fd_ = shm_open(kShmCloudName, O_CREAT | O_RDWR, 0666);
    if (shm_cloud_fd_ < 0) {
      throw std::runtime_error("LatencyPublisher: shm_open(cloud) failed");
    }
    if (ftruncate(shm_cloud_fd_, static_cast<off_t>(shm_cloud_size_)) < 0) {
      throw std::runtime_error("LatencyPublisher: ftruncate(cloud) failed");
    }
    shm_cloud_ptr_ = mmap(nullptr, shm_cloud_size_,
                          PROT_READ | PROT_WRITE, MAP_SHARED, shm_cloud_fd_, 0);
    if (shm_cloud_ptr_ == MAP_FAILED) {
      shm_cloud_ptr_ = nullptr;
      throw std::runtime_error("LatencyPublisher: mmap(cloud) failed");
    }
    RCLCPP_INFO(get_logger(), "SHM cloud region created: %s (%zu bytes)",
                kShmCloudName, shm_cloud_size_);
  }

  RCLCPP_INFO(get_logger(), "LatencyPublisher ready: %u Hz, mode=%d",
              cfg_.rate_hz, static_cast<int>(cfg_.mode));
}

LatencyPublisher::~LatencyPublisher() {
  if (shm_joint_ptr_) {
    munmap(shm_joint_ptr_, sizeof(ShmJointRegion));
    shm_joint_ptr_ = nullptr;
  }
  if (shm_joint_fd_ >= 0) {
    close(shm_joint_fd_);
    shm_unlink(kShmJointName);
    shm_joint_fd_ = -1;
  }
  if (shm_cloud_ptr_) {
    munmap(shm_cloud_ptr_, shm_cloud_size_);
    shm_cloud_ptr_ = nullptr;
  }
  if (shm_cloud_fd_ >= 0) {
    close(shm_cloud_fd_);
    shm_unlink(kShmCloudName);
    shm_cloud_fd_ = -1;
  }
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
  } else if (cfg_.mode == PublishMode::Shm) {
    // Write payload directly into the shared-memory region — no middleware
    // serialization for the joint data itself.
    auto* region = static_cast<ShmJointRegion*>(shm_joint_ptr_);
    region->publish_ns = now_ns;
    region->seq        = static_cast<uint32_t>(seq_);
    std::memcpy(region->joints, joint_msg_.data.data(),
                kJointCount * sizeof(double));

    // Publish a minimal notification so the subscriber's callback fires.
    // Only data[0] (the timestamp) crosses the ROS middleware; the subscriber
    // reads the full joint payload from SHM after receiving this ping.
    std_msgs::msg::Float64MultiArray notif;
    notif.data.resize(1);
    notif.data[0] = ts_as_double;
    joint_pub_->publish(notif);
  } else {
    joint_pub_->publish(joint_msg_);
  }
}

void LatencyPublisher::publish_point_cloud() {
  const int64_t now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();

  if (cfg_.mode == PublishMode::Shm) {
    // Write point data directly into the SHM region.
    auto* hdr = static_cast<ShmCloudHeader*>(shm_cloud_ptr_);
    hdr->publish_ns = now_ns;
    hdr->seq        = static_cast<uint32_t>(seq_);
    hdr->width      = cloud_msg_.width;
    hdr->height     = cloud_msg_.height;
    hdr->point_step = cloud_msg_.point_step;
    std::memcpy(static_cast<char*>(shm_cloud_ptr_) + sizeof(ShmCloudHeader),
                cloud_msg_.data.data(), cloud_msg_.data.size());

    // Publish a minimal notification: header + dimensions, data field empty.
    // The subscriber reads the point payload from SHM; only the header and
    // metadata cross the ROS middleware, keeping serialisation cost O(1).
    sensor_msgs::msg::PointCloud2 notif;
    pack_ns_into_header(notif.header, now_ns);
    notif.header.frame_id = "lidar";
    notif.width      = cloud_msg_.width;
    notif.height     = cloud_msg_.height;
    notif.point_step = cloud_msg_.point_step;
    notif.row_step   = cloud_msg_.row_step;
    notif.fields     = cloud_msg_.fields;
    notif.is_dense   = cloud_msg_.is_dense;
    // notif.data intentionally empty — payload lives in SHM
    cloud_pub_->publish(notif);
  } else {
    pack_ns_into_header(cloud_msg_.header, now_ns);
    cloud_msg_.header.frame_id = "lidar";
    cloud_pub_->publish(cloud_msg_);
  }
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

  if (cfg_.transport_mode == TransportMode::Shm) {
    shm_cloud_size_ = sizeof(ShmCloudHeader) +
        static_cast<std::size_t>(cfg_.cloud_width) *
        static_cast<std::size_t>(cfg_.cloud_height) *
        (3 * sizeof(float));
  }

  RCLCPP_INFO(get_logger(), "LatencySubscriber ready, transport=%d",
              static_cast<int>(cfg_.transport_mode));
}

LatencySubscriber::~LatencySubscriber() {
  if (shm_joint_ptr_) {
    munmap(shm_joint_ptr_, sizeof(ShmJointRegion));
  }
  if (shm_joint_fd_ >= 0) {
    close(shm_joint_fd_);
  }
  if (shm_cloud_ptr_) {
    munmap(shm_cloud_ptr_, shm_cloud_size_);
  }
  if (shm_cloud_fd_ >= 0) {
    close(shm_cloud_fd_);
  }
  // shm_unlink omitted — the publisher owns the regions.
}

void LatencySubscriber::on_joint_state(
    const std_msgs::msg::Float64MultiArray::ConstSharedPtr& msg) {

  const int64_t receive_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();

  if (msg->data.empty()) return;

  int64_t  publish_ns;
  uint32_t data_bytes;

  if (cfg_.transport_mode == TransportMode::Shm) {
    // Lazily map the joint SHM region on the first callback after the
    // publisher has created it. The mapping is reused for all subsequent calls.
    if (!shm_joint_ptr_) {
      const int fd = shm_open("/rt_mw_joint", O_RDONLY, 0);
      if (fd >= 0) {
        void* ptr = mmap(nullptr, sizeof(ShmJointRegion),
                         PROT_READ, MAP_SHARED, fd, 0);
        if (ptr != MAP_FAILED) {
          shm_joint_fd_  = fd;
          shm_joint_ptr_ = ptr;
        } else {
          close(fd);
        }
      }
    }
    if (shm_joint_ptr_) {
      const auto* region = static_cast<const ShmJointRegion*>(shm_joint_ptr_);
      publish_ns = region->publish_ns;
      data_bytes = static_cast<uint32_t>(kJointCount * sizeof(double));
    } else {
      // SHM not ready yet; fall back to the timestamp in the notification.
      std::memcpy(&publish_ns, &msg->data[0], sizeof(publish_ns));
      data_bytes = static_cast<uint32_t>(msg->data.size() * sizeof(double));
    }
  } else {
    std::memcpy(&publish_ns, &msg->data[0], sizeof(publish_ns));
    data_bytes = static_cast<uint32_t>(msg->data.size() * sizeof(double));
  }

  const int64_t latency_ns = receive_ns - publish_ns;

  LatencyRecord rec{
      .timestamp_ns       = publish_ns,
      .message_size_bytes = data_bytes,
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

  int64_t  publish_ns;
  uint32_t data_bytes;

  if (cfg_.transport_mode == TransportMode::Shm) {
    // Lazily map the cloud SHM region on the first callback.
    if (!shm_cloud_ptr_) {
      const int fd = shm_open("/rt_mw_cloud", O_RDONLY, 0);
      if (fd >= 0) {
        void* ptr = mmap(nullptr, shm_cloud_size_,
                         PROT_READ, MAP_SHARED, fd, 0);
        if (ptr != MAP_FAILED) {
          shm_cloud_fd_  = fd;
          shm_cloud_ptr_ = ptr;
        } else {
          close(fd);
        }
      }
    }
    if (shm_cloud_ptr_) {
      const auto* hdr = static_cast<const ShmCloudHeader*>(shm_cloud_ptr_);
      publish_ns = hdr->publish_ns;
      data_bytes = hdr->width * hdr->height * hdr->point_step;
    } else {
      // SHM not ready yet; fall back to the timestamp in the notification header.
      publish_ns = extract_publish_ns(msg->header);
      data_bytes = msg->row_step * msg->height;
    }
  } else {
    publish_ns = extract_publish_ns(msg->header);
    data_bytes = msg->row_step * msg->height;
  }

  const int64_t latency_ns = receive_ns - publish_ns;

  LatencyRecord rec{
      .timestamp_ns       = publish_ns,
      .message_size_bytes = data_bytes,
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
