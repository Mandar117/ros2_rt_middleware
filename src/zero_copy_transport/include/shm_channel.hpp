#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace rt_middleware {

// ── Shared-memory channel ────────────────────────────────────────────────────
//
// Single-writer, multi-reader broadcast ring in POSIX shared memory. Data and
// wake-up both bypass DDS entirely:
//
//   writer: copy payload into slot  →  bump write index  →  futex wake
//   reader: futex wait  →  copy slot out  →  validate slot sequence
//
// Each slot carries a seqlock word: odd while the writer is filling it, and
// 2*(i+1) once it holds sample i. A reader that is lapped by the writer (the
// slot now holds a newer sample, or was rewritten mid-copy) detects it and
// counts an overrun instead of returning torn data. The writer never waits
// for readers, so a slow reader can never stall the publisher.
//
// Wake-up uses a process-shared futex on a counter in the header. The writer
// issues the FUTEX_WAKE syscall only when a reader has announced it is about
// to sleep, so a busy-polling reader costs the writer nothing.

inline constexpr std::size_t kShmMaxPayload = 4096;   // bytes per sample
inline constexpr std::size_t kShmSlotCount  = 64;     // ring depth
inline constexpr char        kShmDefaultName[] = "/rt_mw_channel";

// Sample as seen by a reader (copied out of shared memory).
struct ShmSample {
  int64_t  publish_ns{0};
  uint64_t seq{0};
  uint32_t type{0};
  uint32_t size{0};
  alignas(8) std::byte payload[kShmMaxPayload];
};

class ShmChannelWriter {
public:
  // Creates the region (removing a stale one with the same name).
  // Throws std::runtime_error on failure.
  explicit ShmChannelWriter(const std::string& name = kShmDefaultName);
  ~ShmChannelWriter();  // marks the region closed, wakes readers, unlinks

  ShmChannelWriter(const ShmChannelWriter&) = delete;
  ShmChannelWriter& operator=(const ShmChannelWriter&) = delete;

  // Publish one sample. Never blocks; size must be <= kShmMaxPayload.
  void write(uint32_t type, int64_t publish_ns, uint64_t seq,
             const void* payload, uint32_t size) noexcept;

  uint64_t written() const noexcept { return next_idx_; }

private:
  std::string name_;
  int         fd_{-1};
  void*       base_{nullptr};
  std::size_t size_{0};
  uint64_t    next_idx_{0};
};

class ShmChannelReader {
public:
  enum class Result { Ok, Timeout, Closed, NotOpen };

  explicit ShmChannelReader(std::string name = kShmDefaultName,
                            int64_t spin_before_wait_ns = 0);
  ~ShmChannelReader();

  ShmChannelReader(const ShmChannelReader&) = delete;
  ShmChannelReader& operator=(const ShmChannelReader&) = delete;

  // Map the region if the writer has created it. Starts reading at the
  // writer's current position (backlog is skipped). Returns true when open.
  bool try_open();
  bool is_open() const noexcept { return base_ != nullptr; }
  void close();

  // Wait up to `timeout` for the next sample and copy it into `out`.
  // Busy-polls for spin_before_wait_ns before sleeping on the futex.
  Result read(ShmSample& out, std::chrono::nanoseconds timeout);

  // Samples lost because the writer lapped this reader.
  uint64_t overruns() const noexcept { return overruns_; }

private:
  bool try_read_one(ShmSample& out);

  std::string name_;
  int64_t     spin_before_wait_ns_;
  int         fd_{-1};
  void*       base_{nullptr};
  std::size_t size_{0};
  uint64_t    read_idx_{0};
  uint64_t    overruns_{0};
};

}  // namespace rt_middleware
