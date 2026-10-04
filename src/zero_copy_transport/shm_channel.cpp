#include "shm_channel.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <new>
#include <stdexcept>

#include <fcntl.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace rt_middleware {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint64_t kMagic   = 0x524D5753484D3031ULL;  // "RMWSHM01"
constexpr uint32_t kVersion = 1;

// Atomics in the mapping are shared across processes; that is only valid for
// lock-free (address-free) atomics.
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t),
              "futex word must be a plain 32-bit integer");

struct alignas(64) ShmHeader {
  std::atomic<uint64_t> magic;     // written last by the writer (release)
  uint32_t version;
  uint32_t slot_count;
  uint32_t max_payload;
  uint32_t _pad0;

  alignas(64) std::atomic<uint64_t> write_idx;   // samples published so far
  alignas(64) std::atomic<uint32_t> futex_word;  // bumped on every publish
  std::atomic<uint32_t> waiters;                 // readers about to sleep
  std::atomic<uint32_t> closed;                  // writer has gone away
};

struct alignas(64) ShmSlot {
  std::atomic<uint64_t> seqlock;   // odd = being written; 2*(i+1) = holds sample i
  int64_t  publish_ns;
  uint64_t seq;
  uint32_t type;
  uint32_t size;
  alignas(8) std::byte payload[kShmMaxPayload];
};

constexpr std::size_t region_size() {
  return sizeof(ShmHeader) + kShmSlotCount * sizeof(ShmSlot);
}

ShmHeader* header_of(void* base) { return static_cast<ShmHeader*>(base); }

ShmSlot* slot_of(void* base, uint64_t idx) {
  auto* slots = reinterpret_cast<ShmSlot*>(static_cast<std::byte*>(base) + sizeof(ShmHeader));
  return &slots[idx % kShmSlotCount];
}

// Process-shared futex (no FUTEX_PRIVATE_FLAG — the word lives in a shared mapping).
long futex_wait(std::atomic<uint32_t>* word, uint32_t expected, const timespec* rel_timeout) {
  return syscall(SYS_futex, reinterpret_cast<uint32_t*>(word), FUTEX_WAIT,
                 expected, rel_timeout, nullptr, 0);
}

long futex_wake_all(std::atomic<uint32_t>* word) {
  return syscall(SYS_futex, reinterpret_cast<uint32_t*>(word), FUTEX_WAKE,
                 INT_MAX, nullptr, nullptr, 0);
}

}  // namespace

// ── Writer ───────────────────────────────────────────────────────────────────

ShmChannelWriter::ShmChannelWriter(const std::string& name) : name_(name) {
  // A previous writer that crashed leaves its region behind; start fresh so
  // readers never attach to stale data.
  shm_unlink(name_.c_str());

  fd_ = shm_open(name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0660);
  if (fd_ < 0) {
    throw std::runtime_error("ShmChannelWriter: shm_open(" + name_ + ") failed: " +
                             std::strerror(errno));
  }
  size_ = region_size();
  if (ftruncate(fd_, static_cast<off_t>(size_)) < 0) {
    const int err = errno;
    ::close(fd_);
    shm_unlink(name_.c_str());
    throw std::runtime_error(std::string("ShmChannelWriter: ftruncate failed: ") +
                             std::strerror(err));
  }
  base_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  if (base_ == MAP_FAILED) {
    const int err = errno;
    base_ = nullptr;
    ::close(fd_);
    shm_unlink(name_.c_str());
    throw std::runtime_error(std::string("ShmChannelWriter: mmap failed: ") +
                             std::strerror(err));
  }

  // ftruncate zero-fills, so every slot's seqlock already reads 0 ("empty").
  auto* hdr = new (base_) ShmHeader{};
  hdr->version     = kVersion;
  hdr->slot_count  = static_cast<uint32_t>(kShmSlotCount);
  hdr->max_payload = static_cast<uint32_t>(kShmMaxPayload);
  hdr->write_idx.store(0, std::memory_order_relaxed);
  hdr->futex_word.store(0, std::memory_order_relaxed);
  hdr->waiters.store(0, std::memory_order_relaxed);
  hdr->closed.store(0, std::memory_order_relaxed);
  // Publish the initialised header: readers check magic with acquire.
  hdr->magic.store(kMagic, std::memory_order_release);
}

ShmChannelWriter::~ShmChannelWriter() {
  if (base_) {
    auto* hdr = header_of(base_);
    hdr->closed.store(1, std::memory_order_seq_cst);
    hdr->futex_word.fetch_add(1, std::memory_order_seq_cst);
    futex_wake_all(&hdr->futex_word);
    munmap(base_, size_);
  }
  if (fd_ >= 0) {
    // Only unlink the name if it still refers to our region — a newer writer
    // may have replaced it, and we must not pull it out from under its readers.
    struct stat ours{};
    struct stat current{};
    const int cur_fd = shm_open(name_.c_str(), O_RDONLY, 0);
    if (cur_fd >= 0) {
      if (fstat(fd_, &ours) == 0 && fstat(cur_fd, &current) == 0 &&
          ours.st_ino == current.st_ino && ours.st_dev == current.st_dev) {
        shm_unlink(name_.c_str());
      }
      ::close(cur_fd);
    }
    ::close(fd_);
  }
}

void ShmChannelWriter::write(uint32_t type, int64_t publish_ns, uint64_t seq,
                             const void* payload, uint32_t size) noexcept {
  auto* hdr  = header_of(base_);
  const uint64_t idx = next_idx_++;
  ShmSlot* slot = slot_of(base_, idx);
  size = std::min<uint32_t>(size, static_cast<uint32_t>(kShmMaxPayload));

  // Seqlock write: mark odd, fill, mark with the sample's even sequence.
  slot->seqlock.store(2 * idx + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  slot->publish_ns = publish_ns;
  slot->seq        = seq;
  slot->type       = type;
  slot->size       = size;
  std::memcpy(slot->payload, payload, size);
  slot->seqlock.store(2 * (idx + 1), std::memory_order_release);

  // Make the sample visible, then wake sleepers. seq_cst on futex_word and
  // waiters pairs with the reader's waiters++ / write_idx re-check so a reader
  // can never sleep through a publish (Dekker-style handshake).
  hdr->write_idx.store(idx + 1, std::memory_order_seq_cst);
  hdr->futex_word.fetch_add(1, std::memory_order_seq_cst);
  if (hdr->waiters.load(std::memory_order_seq_cst) != 0) {
    futex_wake_all(&hdr->futex_word);
  }
}

// ── Reader ───────────────────────────────────────────────────────────────────

ShmChannelReader::ShmChannelReader(std::string name, int64_t spin_before_wait_ns)
    : name_(std::move(name)), spin_before_wait_ns_(spin_before_wait_ns) {}

ShmChannelReader::~ShmChannelReader() { close(); }

bool ShmChannelReader::try_open() {
  if (base_) return true;

  const int fd = shm_open(name_.c_str(), O_RDWR, 0);
  if (fd < 0) return false;

  struct stat st{};
  if (fstat(fd, &st) < 0 || static_cast<std::size_t>(st.st_size) < region_size()) {
    ::close(fd);  // writer still initialising (ftruncate not done yet)
    return false;
  }
  void* base = mmap(nullptr, region_size(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (base == MAP_FAILED) {
    ::close(fd);
    return false;
  }
  auto* hdr = header_of(base);
  if (hdr->magic.load(std::memory_order_acquire) != kMagic ||
      hdr->version != kVersion || hdr->slot_count != kShmSlotCount ||
      hdr->max_payload != kShmMaxPayload ||
      hdr->closed.load(std::memory_order_acquire) != 0) {
    munmap(base, region_size());
    ::close(fd);
    return false;
  }

  fd_       = fd;
  base_     = base;
  size_     = region_size();
  read_idx_ = hdr->write_idx.load(std::memory_order_acquire);
  return true;
}

void ShmChannelReader::close() {
  if (base_) {
    munmap(base_, size_);
    base_ = nullptr;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

bool ShmChannelReader::try_read_one(ShmSample& out) {
  auto* hdr = header_of(base_);
  while (true) {
    const uint64_t w = hdr->write_idx.load(std::memory_order_acquire);
    if (read_idx_ >= w) {
      return false;
    }
    // Writer is a full ring ahead: everything older than w - N is gone.
    if (w - read_idx_ > kShmSlotCount) {
      overruns_ += (w - read_idx_) - kShmSlotCount;
      read_idx_  = w - kShmSlotCount;
    }

    const ShmSlot* slot = slot_of(base_, read_idx_);
    const uint64_t expected = 2 * (read_idx_ + 1);
    const uint64_t s1 = slot->seqlock.load(std::memory_order_acquire);
    if (s1 != expected) {
      // Slot already recycled for a newer sample (or being rewritten).
      ++overruns_;
      ++read_idx_;
      continue;
    }

    out.publish_ns = slot->publish_ns;
    out.seq        = slot->seq;
    out.type       = slot->type;
    out.size       = std::min<uint32_t>(slot->size, static_cast<uint32_t>(kShmMaxPayload));
    std::memcpy(out.payload, slot->payload, out.size);

    std::atomic_thread_fence(std::memory_order_acquire);
    const uint64_t s2 = slot->seqlock.load(std::memory_order_relaxed);
    ++read_idx_;
    if (s2 != s1) {
      ++overruns_;  // writer lapped us mid-copy; data is torn — discard
      continue;
    }
    return true;
  }
}

ShmChannelReader::Result ShmChannelReader::read(ShmSample& out,
                                                std::chrono::nanoseconds timeout) {
  if (!base_) return Result::NotOpen;
  auto* hdr = header_of(base_);

  const auto deadline   = Clock::now() + timeout;
  const auto spin_until = Clock::now() + std::chrono::nanoseconds(spin_before_wait_ns_);

  while (true) {
    if (try_read_one(out)) return Result::Ok;
    if (hdr->closed.load(std::memory_order_acquire) != 0) return Result::Closed;

    const auto now = Clock::now();
    if (now >= deadline) return Result::Timeout;
    if (now < spin_until) continue;  // busy-poll phase

    // Announce we're about to sleep, then re-check before blocking. If the
    // writer bumps futex_word after we sampled it, FUTEX_WAIT returns at once.
    const uint32_t observed = hdr->futex_word.load(std::memory_order_seq_cst);
    hdr->waiters.fetch_add(1, std::memory_order_seq_cst);
    if (hdr->write_idx.load(std::memory_order_seq_cst) > read_idx_ ||
        hdr->closed.load(std::memory_order_seq_cst) != 0) {
      hdr->waiters.fetch_sub(1, std::memory_order_seq_cst);
      continue;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
    timespec ts{};
    ts.tv_sec  = static_cast<time_t>(remaining / 1'000'000'000LL);
    ts.tv_nsec = static_cast<long>(remaining % 1'000'000'000LL);
    futex_wait(&hdr->futex_word, observed, &ts);  // EAGAIN/EINTR/ETIMEDOUT → loop
    hdr->waiters.fetch_sub(1, std::memory_order_seq_cst);
  }
}

}  // namespace rt_middleware
