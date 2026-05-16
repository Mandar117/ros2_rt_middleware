#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace rt_middleware {

// Single-producer, single-consumer (SPSC) lock-free ring buffer.
//
// Design constraints that matter for real-time:
//   - Zero heap allocation after construction
//   - No mutexes — sequencing is enforced by acquire/release on head/tail
//   - head_ and tail_ on separate cache lines to prevent false sharing
//   - Capacity must be a power of 2 so modulo reduces to a bitmask
//
// Thread-safety: push() may only be called from ONE thread (producer).
//                pop() may only be called from ONE thread (consumer).
//                These may be different threads simultaneously.
//
// The buffer holds at most (Capacity - 1) elements. One slot is sacrificed
// to distinguish full from empty without a separate counter.

template <typename T, std::size_t Capacity>
class LockFreeRingBuffer {
  static_assert((Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of 2");
  static_assert(Capacity >= 2, "Capacity must be at least 2");
  static_assert(std::is_default_constructible_v<T>,
                "T must be default-constructible");

  static constexpr std::size_t kMask = Capacity - 1;
  static constexpr std::size_t kCacheLineSize = 64;

  // Pad each atomic to its own cache line to prevent false sharing between
  // the producer (writing head_) and the consumer (writing tail_).
  struct alignas(kCacheLineSize) PaddedAtomic {
    std::atomic<std::size_t> v{0};
    char pad[kCacheLineSize - sizeof(std::atomic<std::size_t>)];
  };

  std::array<T, Capacity> data_{};
  PaddedAtomic head_{};  // next write slot — owned by producer
  PaddedAtomic tail_{};  // next read  slot — owned by consumer

public:
  LockFreeRingBuffer() = default;

  // Non-copyable, non-movable — the ring buffer must stay in place.
  LockFreeRingBuffer(const LockFreeRingBuffer&) = delete;
  LockFreeRingBuffer& operator=(const LockFreeRingBuffer&) = delete;

  // --- Producer API --------------------------------------------------------

  // Push an item. Returns false if the buffer is full (caller must retry or
  // drop). Does NOT block.
  bool push(T item) noexcept(std::is_nothrow_move_assignable_v<T>) {
    const std::size_t h = head_.v.load(std::memory_order_relaxed);
    const std::size_t next_h = (h + 1) & kMask;

    // If next_h == tail, the buffer is full.
    // Acquire the tail so we see all consumer writes up to this point.
    if (next_h == tail_.v.load(std::memory_order_acquire)) {
      return false;
    }

    data_[h] = std::move(item);

    // Release so the consumer sees the written data before the head update.
    head_.v.store(next_h, std::memory_order_release);
    return true;
  }

  // --- Consumer API --------------------------------------------------------

  // Pop an item into `out`. Returns false if the buffer is empty.
  bool pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
    const std::size_t t = tail_.v.load(std::memory_order_relaxed);

    // Acquire the head so we see all producer writes up to this point.
    if (t == head_.v.load(std::memory_order_acquire)) {
      return false;
    }

    out = std::move(data_[t]);

    // Release so the producer sees the freed slot.
    tail_.v.store((t + 1) & kMask, std::memory_order_release);
    return true;
  }

  // --- Query API (approximate under concurrent access) ---------------------

  bool empty() const noexcept {
    return tail_.v.load(std::memory_order_acquire) ==
           head_.v.load(std::memory_order_acquire);
  }

  // Maximum elements the buffer can hold (one slot is reserved).
  static constexpr std::size_t max_size() noexcept { return Capacity - 1; }

  // Approximate number of elements currently queued.
  std::size_t size_approx() const noexcept {
    const std::size_t h = head_.v.load(std::memory_order_acquire);
    const std::size_t t = tail_.v.load(std::memory_order_acquire);
    return (h - t + Capacity) & kMask;
  }
};

}  // namespace rt_middleware
