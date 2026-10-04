#include "csv_logger.hpp"

#include <chrono>
#include <stdexcept>

namespace rt_middleware {

CsvLogger::CsvLogger(const std::string& path)
    : file_(path, std::ios::out | std::ios::trunc) {
  if (!file_.is_open()) {
    throw std::runtime_error("CsvLogger: cannot open " + path);
  }
  file_ << "timestamp_ns,message_size_bytes,transport_mode,"
           "publish_rate_hz,latency_ns,message_type,seq\n";
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
    write_row(rec);
  }
  file_.flush();
}

bool CsvLogger::log(const LatencyRecord& rec) noexcept {
  if (buffer_.push(rec)) {
    return true;
  }
  dropped_.fetch_add(1, std::memory_order_relaxed);
  return false;
}

void CsvLogger::write_row(const LatencyRecord& rec) {
  file_ << rec.timestamp_ns << ','
        << rec.message_size_bytes << ','
        << static_cast<int>(rec.transport_mode) << ','
        << rec.publish_rate_hz << ','
        << rec.latency_ns << ','
        << static_cast<int>(rec.message_type) << ','
        << rec.seq << '\n';
}

void CsvLogger::writer_loop() {
  LatencyRecord rec{};
  while (running_.load(std::memory_order_acquire)) {
    bool wrote = false;
    while (buffer_.pop(rec)) {
      write_row(rec);
      wrote = true;
    }
    if (wrote) {
      file_.flush();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

}  // namespace rt_middleware
