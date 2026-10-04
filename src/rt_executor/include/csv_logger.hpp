#pragma once

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>

#include "lock_free_ring_buffer.hpp"

namespace rt_middleware {

enum class TransportMode : uint8_t {
  Copy   = 0,
  Loaned = 1,
  Shm    = 2,
};

enum class MessageType : uint8_t {
  Joint = 0,   // JointSample — 96-byte payload
  Cloud = 1,   // CloudSample — 3840-byte payload
};

struct LatencyRecord {
  int64_t       timestamp_ns;       // steady_clock at publish
  uint32_t      message_size_bytes; // payload bytes
  TransportMode transport_mode;
  uint32_t      publish_rate_hz;
  int64_t       latency_ns;         // one-way: receive_time - publish_time
  MessageType   message_type;
  uint64_t      seq;                // publisher sequence number
};

// Lock-free CSV logger.
//
// The hot path (log()) pushes a LatencyRecord into an SPSC ring buffer without
// blocking. A background writer thread drains the buffer to disk so that
// file I/O never stalls the subscriber callback.
//
// log() must be called from a single thread (SPSC). If the buffer is full the
// record is dropped and counted in dropped().
//
// CSV columns:
//   timestamp_ns,message_size_bytes,transport_mode,publish_rate_hz,
//   latency_ns,message_type,seq
class CsvLogger {
public:
  static constexpr std::size_t kBufferSize = 8192;

  explicit CsvLogger(const std::string& path);
  ~CsvLogger();  // drains everything still buffered, then closes the file

  CsvLogger(const CsvLogger&) = delete;
  CsvLogger& operator=(const CsvLogger&) = delete;

  // Non-blocking. Returns false (and counts a drop) if the buffer is full.
  bool log(const LatencyRecord& rec) noexcept;

  uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
  void writer_loop();
  void write_row(const LatencyRecord& rec);

  LockFreeRingBuffer<LatencyRecord, kBufferSize> buffer_;
  std::ofstream file_;
  std::thread writer_thread_;
  std::atomic<bool> running_{true};
  std::atomic<uint64_t> dropped_{0};
};

}  // namespace rt_middleware
