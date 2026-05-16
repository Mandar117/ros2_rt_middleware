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

struct LatencyRecord {
  int64_t      timestamp_ns;       // steady_clock epoch at publish
  uint32_t     message_size_bytes;
  TransportMode transport_mode;
  uint32_t     publish_rate_hz;
  int64_t      latency_ns;         // one-way: subscribe_time - publish_time
};

// Lock-free CSV logger.
//
// The hot path (log()) pushes a LatencyRecord into a ring buffer without
// blocking. A background writer thread drains the buffer to disk so that
// file I/O never stalls the publisher or subscriber callbacks.
//
// If the buffer fills up (publisher produces faster than the writer consumes)
// log() returns false and the record is silently dropped. In practice the
// 8192-slot buffer handles bursts at 1 kHz comfortably.
class CsvLogger {
public:
  static constexpr std::size_t kBufferSize = 8192;

  explicit CsvLogger(const std::string& path);
  ~CsvLogger();

  // Non-blocking. Returns false if the ring buffer is full.
  bool log(const LatencyRecord& rec) noexcept;

private:
  void writer_loop();

  LockFreeRingBuffer<LatencyRecord, kBufferSize> buffer_;
  std::ofstream file_;
  std::thread writer_thread_;
  std::atomic<bool> running_{true};
};

}  // namespace rt_middleware
