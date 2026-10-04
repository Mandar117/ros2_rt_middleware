#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <ostream>
#include <vector>

namespace rt_middleware {

// Latency distribution for one metric. All values in nanoseconds.
struct JitterStats {
  int64_t  min_ns{0};
  int64_t  max_ns{0};
  int64_t  p50_ns{0};
  int64_t  p99_ns{0};
  int64_t  p999_ns{0};
  double   mean_ns{0.0};
  uint64_t total_callbacks{0};     // every event, including unrecorded ones
  uint64_t recorded_samples{0};    // min(total_callbacks, capacity)
  uint64_t missed_deadlines{0};
};

// Fixed-capacity sample store. Storage is reserved at construction, so
// record() never allocates and is safe on a real-time path. Samples are kept
// in arrival order (the first `capacity` events); total() keeps counting
// after the store is full.
//
// Not thread-safe: record() must be called from a single thread, and the
// accessors only after that thread has stopped recording.
class SampleSet {
public:
  explicit SampleSet(std::size_t capacity) { samples_.reserve(capacity); }

  void record(int64_t v) noexcept {
    ++total_;
    if (samples_.size() < samples_.capacity()) samples_.push_back(v);
  }

  uint64_t total() const noexcept { return total_; }
  const std::vector<int64_t>& samples() const noexcept { return samples_; }

  JitterStats compute(uint64_t missed) const {
    JitterStats stats;
    stats.total_callbacks  = total_;
    stats.recorded_samples = samples_.size();
    stats.missed_deadlines = missed;
    if (samples_.empty()) {
      return stats;
    }

    // Work on a sorted copy — keep the arrival order intact for export.
    std::vector<int64_t> sorted(samples_);
    std::sort(sorted.begin(), sorted.end());

    const auto at = [&sorted](double q) {
      return sorted[static_cast<std::size_t>(q * static_cast<double>(sorted.size() - 1))];
    };
    stats.min_ns  = sorted.front();
    stats.max_ns  = sorted.back();
    stats.p50_ns  = at(0.50);
    stats.p99_ns  = at(0.99);
    stats.p999_ns = at(0.999);
    stats.mean_ns = static_cast<double>(
                        std::accumulate(sorted.begin(), sorted.end(), int64_t{0})) /
                    static_cast<double>(sorted.size());
    return stats;
  }

  // Append rows "<metric>,<value_ns>" in arrival order.
  void write_csv_rows(std::ostream& out, const char* metric) const {
    for (int64_t v : samples_) out << metric << ',' << v << '\n';
  }

private:
  std::vector<int64_t> samples_;
  uint64_t total_{0};
};

}  // namespace rt_middleware
