#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>

namespace rt_middleware {

// Fixed-size, lock-free memory pool allocator.
//
// All memory is pre-allocated in a contiguous array at construction time.
// allocate() and deallocate() never touch the heap after that point —
// critical for avoiding latency spikes from the OS memory allocator in the
// real-time hot path.
//
// ABA prevention: The freelist uses a 64-bit tagged pointer where the upper
// 32 bits are a monotonically increasing version counter. This prevents the
// classic ABA scenario where a slot is freed and reallocated before a
// concurrent CAS re-reads the head.
//
// Thread-safety: allocate() and deallocate() are safe to call from multiple
// threads concurrently.

template <typename T, std::size_t PoolSize>
class MemoryPool {
  static_assert(PoolSize > 0, "PoolSize must be positive");
  static_assert(PoolSize <= static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()),
                "PoolSize must fit in uint32_t");

  // Each slot in the pool is a raw storage block aligned for T.
  // When a slot is free, it also holds the freelist index link in a
  // separate parallel array (next_) so we never alias live T objects.
  struct alignas(alignof(T)) Slot {
    std::byte storage[sizeof(T)];
  };

  std::array<Slot, PoolSize> slots_{};

  // next_[i] = index of the next free slot after slot i, or kNull when none.
  static constexpr uint32_t kNull = std::numeric_limits<uint32_t>::max();
  std::array<std::atomic<uint32_t>, PoolSize> next_{};

  // Tagged freelist head: high 32 bits = version, low 32 bits = slot index
  // (or kNull). Packed into a single 64-bit atomic for ABA-safe CAS.
  std::atomic<uint64_t> free_head_{0};  // version=0, index=0 initially

  std::atomic<uint32_t> available_{static_cast<uint32_t>(PoolSize)};

  static constexpr uint64_t pack(uint32_t version, uint32_t index) noexcept {
    return (static_cast<uint64_t>(version) << 32) | static_cast<uint64_t>(index);
  }
  static constexpr uint32_t version_of(uint64_t tagged) noexcept {
    return static_cast<uint32_t>(tagged >> 32);
  }
  static constexpr uint32_t index_of(uint64_t tagged) noexcept {
    return static_cast<uint32_t>(tagged & 0xFFFF'FFFF);
  }

public:
  MemoryPool() {
    // Build the initial freelist: slot 0 → 1 → 2 → … → PoolSize-1 → kNull
    for (uint32_t i = 0; i + 1 < static_cast<uint32_t>(PoolSize); ++i) {
      next_[i].store(i + 1, std::memory_order_relaxed);
    }
    next_[PoolSize - 1].store(kNull, std::memory_order_relaxed);
    // Head starts at slot 0, version 0.
    free_head_.store(pack(0, 0), std::memory_order_release);
  }

  MemoryPool(const MemoryPool&) = delete;
  MemoryPool& operator=(const MemoryPool&) = delete;

  // Returns a pointer to raw storage for one T, or nullptr if the pool is
  // exhausted. The caller is responsible for placement-new before use and
  // explicit destruction before deallocate().
  T* allocate() noexcept {
    uint64_t head = free_head_.load(std::memory_order_acquire);
    while (true) {
      const uint32_t idx = index_of(head);
      if (idx == kNull) {
        return nullptr;  // pool exhausted
      }
      const uint32_t next_idx = next_[idx].load(std::memory_order_relaxed);
      const uint32_t new_ver = version_of(head) + 1;
      const uint64_t new_head = pack(new_ver, next_idx);

      if (free_head_.compare_exchange_weak(head, new_head,
                                           std::memory_order_release,
                                           std::memory_order_acquire)) {
        available_.fetch_sub(1, std::memory_order_relaxed);
        return reinterpret_cast<T*>(&slots_[idx]);
      }
      // head was updated by compare_exchange_weak on failure — retry
    }
  }

  // Return a previously allocated slot to the pool. ptr must have been
  // obtained from allocate() on this pool and the object at ptr must have
  // already been destroyed by the caller.
  void deallocate(T* ptr) noexcept {
    if (!ptr) return;

    auto* slot = reinterpret_cast<Slot*>(ptr);
    const ptrdiff_t diff = slot - slots_.data();
    assert(diff >= 0 && static_cast<std::size_t>(diff) < PoolSize &&
           "pointer does not belong to this pool");
    const uint32_t idx = static_cast<uint32_t>(diff);

    uint64_t head = free_head_.load(std::memory_order_acquire);
    while (true) {
      next_[idx].store(index_of(head), std::memory_order_relaxed);
      const uint32_t new_ver = version_of(head) + 1;
      const uint64_t new_head = pack(new_ver, idx);

      if (free_head_.compare_exchange_weak(head, new_head,
                                           std::memory_order_release,
                                           std::memory_order_acquire)) {
        available_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  }

  std::size_t available() const noexcept {
    return available_.load(std::memory_order_relaxed);
  }

  static constexpr std::size_t capacity() noexcept { return PoolSize; }
};

}  // namespace rt_middleware
