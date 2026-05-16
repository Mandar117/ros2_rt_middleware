# ROS2 Real-Time Middleware — Custom Executor & Latency Benchmarking Suite

[![CI](https://github.com/Mandar117/ros2_rt_middleware/actions/workflows/ci.yml/badge.svg)](https://github.com/Mandar117/ros2_rt_middleware/actions/workflows/ci.yml)

Production-grade ROS2 middleware component in C++20 demonstrating real-time
performance engineering: lock-free data structures, SCHED_FIFO thread
scheduling, CPU affinity, zero-copy transport, and a full latency
benchmarking suite with Python visualisation.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                        ros2_rt_middleware                           │
│                                                                     │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │                   rt_executor  (shared lib)                   │  │
│  │                                                               │  │
│  │  ┌─────────────────────┐   ┌───────────────────────────────┐ │  │
│  │  │  LockFreeRingBuffer │   │       MemoryPool<T, N>        │ │  │
│  │  │  (SPSC, power-of-2) │   │  (tagged-ptr ABA-safe CAS)    │ │  │
│  │  │  head_ ──► tail_    │   │  pre-allocated at ctor time   │ │  │
│  │  │  cache-line padded  │   │  zero heap in hot path        │ │  │
│  │  └─────────────────────┘   └───────────────────────────────┘ │  │
│  │                │                           │                  │  │
│  │                └──────────┬────────────────┘                  │  │
│  │                           ▼                                   │  │
│  │                    RTExecutor::spin()                         │  │
│  │                    ├─ pthread_setaffinity_np(cpu_core)        │  │
│  │                    ├─ SCHED_FIFO / SCHED_RR (CAP_SYS_NICE)   │  │
│  │                    ├─ busy-wait or nanosleep polling          │  │
│  │                    └─ per-callback jitter measurement         │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌────────────────────────────┐  ┌──────────────────────────────┐  │
│  │      latency_publisher     │  │     latency_subscriber       │  │
│  │                            │  │                              │  │
│  │  Publishes every 1/rate s  │  │  Receives, extracts stamp,   │  │
│  │  ┌────────────────────┐    │  │  computes one-way latency,   │  │
│  │  │ Float64MultiArray  │    │  │  feeds CsvLogger (lock-free  │  │
│  │  │ 12 doubles (96 B)  │    │  │  ring buffer → background    │  │
│  │  │ ts in data[0]      │    │  │  writer thread).             │  │
│  │  ├────────────────────┤    │  └──────────────────────────────┘  │
│  │  │ PointCloud2        │    │                                    │
│  │  │ 320×1 xyz floats   │    │  Transport modes:                  │
│  │  │ ts in header.stamp │    │    0 = standard copy              │
│  │  └────────────────────┘    │    1 = loaned message API         │
│  │                            │    2 = POSIX SHM fallback         │
│  │  Modes: Copy / Loaned / SHM│                                    │
│  └────────────────────────────┘                                    │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                   Python analysis layer                      │   │
│  │  plot_latency.py: CDF · latency-vs-rate · jitter bar chart  │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

### Message flow (one-way latency measurement)

```
Publisher                                     Subscriber
   │                                               │
   │  t₀ = steady_clock::now()                    │
   │  pack t₀ → message payload / header           │
   │  publish(msg)  ─────────────────────────────► │
   │                                               │  t₁ = steady_clock::now()  ← first line of callback
   │                                               │  latency = t₁ − t₀
   │                                               │  CsvLogger::log(record)    ← lock-free push
```

Both nodes run on the same host, so `steady_clock` is the same reference.
For cross-host scenarios, replace with a PTP-synced hardware clock.

---

## Repository Structure

```
ros2_rt_middleware/
├── src/
│   ├── rt_executor/
│   │   ├── include/
│   │   │   ├── lock_free_ring_buffer.hpp  # SPSC ring buffer (header-only)
│   │   │   ├── memory_pool.hpp            # Fixed-size pool (header-only)
│   │   │   ├── rt_executor.hpp            # Custom executor interface
│   │   │   └── csv_logger.hpp             # Lock-free CSV logger interface
│   │   └── rt_executor.cpp                # RTExecutor implementation
│   ├── zero_copy_transport/
│   │   ├── include/
│   │   │   ├── latency_publisher.hpp
│   │   │   └── latency_subscriber.hpp
│   │   ├── latency_impl.cpp               # Publisher, subscriber, CsvLogger impl
│   │   ├── latency_publisher_node.cpp     # Publisher main()
│   │   └── latency_subscriber_node.cpp    # Subscriber main()
│   └── benchmarks/
│       └── benchmark_runner.cpp           # Orchestrates full matrix
├── test/
│   ├── test_ring_buffer.cpp               # 10 gtest cases incl. SPSC thread test
│   ├── test_memory_pool.cpp               # Concurrent alloc/dealloc stress
│   └── test_executor_ordering.cpp         # FIFO ordering, stop, back-pressure
├── scripts/
│   ├── plot_latency.py                    # Matplotlib: CDF, line chart, bar
│   └── run_benchmarks.sh                  # Shell driver for full matrix
├── results/                               # CSV outputs + generated PNGs
├── .github/
│   └── workflows/
│       └── ci.yml                         # GitHub Actions: build + 29 unit tests
├── CMakeLists.txt
└── package.xml
```

---

## Build

```bash
# Source ROS2 Humble
source /opt/ros/humble/setup.bash

# From the workspace root (one level above ros2_rt_middleware/)
colcon build --packages-select ros2_rt_middleware \
             --cmake-args -DCMAKE_BUILD_TYPE=Release

source install/setup.bash
```

### Enable real-time scheduling without root (recommended)

```bash
# Grant CAP_SYS_NICE to the publisher binary so it can call
# pthread_setschedparam without sudo.
sudo setcap cap_sys_nice+ep \
    install/lib/ros2_rt_middleware/latency_publisher

sudo setcap cap_sys_nice+ep \
    install/lib/ros2_rt_middleware/benchmark_runner
```

Alternatively, add your user to the `realtime` group and configure
`/etc/security/limits.d/99-realtime.conf`:

```
@realtime   -   rtprio   99
@realtime   -   memlock  unlimited
```

---

## Run Benchmarks

### Option A — automated full matrix

```bash
# Runs all 9 combinations (3 modes × 3 rates), 10 s each, then plots.
DURATION=10 CPU_CORE=3 bash scripts/run_benchmarks.sh
```

### Option B — manual per-pair

Terminal 1 (subscriber, records latency to CSV):
```bash
ros2 run ros2_rt_middleware latency_subscriber \
    --rate 1000 --mode copy --output results/latency_copy_1000Hz.csv
```

Terminal 2 (publisher):
```bash
ros2 run ros2_rt_middleware latency_publisher \
    --rate 1000 --mode copy
```

### Option C — all-in-one benchmark_runner

```bash
ros2 run ros2_rt_middleware benchmark_runner \
    --duration 10 --core 3
```

### Generate plots

```bash
pip install matplotlib pandas numpy
python3 scripts/plot_latency.py --results results/ --output results/
```

---

## Run Tests

```bash
colcon test --packages-select ros2_rt_middleware
colcon test-result --verbose
```

Test suite covers:

| Test file | Cases |
|---|---|
| `test_ring_buffer` | Empty/push/pop, FIFO, wrap-around, full/empty guards, SPSC 100 k-item throughput |
| `test_memory_pool` | Exhaustion, recycling, pointer uniqueness, concurrent stress (4 threads × 10 k ops) |
| `test_executor_ordering` | FIFO dispatch, stop signal, queue back-pressure, jitter stats |

---

## Results

Measured on **WSL2 / Ubuntu 22.04 / ROS2 Humble** (Windows 11 host, standard
kernel — no `PREEMPT_RT`, no `SCHED_FIFO` due to WSL capability restrictions).
~2 000–20 000 samples per scenario. Plots in `results/`.

### One-way message latency (publisher → subscriber, same host)

| Transport | Rate (Hz) | Samples | p50 (µs) | p99 (µs) | max (µs) |
|-----------|----------:|--------:|---------:|---------:|---------:|
| Copy      | 100       |   1 998 |    130.4 |    345.4 |    432.4 |
| Copy      | 500       |  10 020 |    153.9 |    325.8 |    976.2 |
| Copy      | 1000      |  19 948 |     93.7 |    320.4 |    379.4 |
| Loaned    | 100       |   2 002 |    143.3 |    363.0 |    494.2 |
| Loaned    | 500       |  10 002 |    154.7 |    324.5 |    450.4 |
| Loaned    | 1000      |  19 987 |     97.3 |    322.5 |    417.4 |
| SHM       | 100       |   2 002 |    148.9 |    350.2 |    436.9 |
| SHM       | 500       |   9 980 |    151.5 |    327.8 |    487.5 |
| SHM       | 1000      |  20 012 |     90.8 |    320.7 |    556.7 |

> **Note:** Loaned-message mode fell back to local allocator (FastDDS does not
> expose the loaned-message API by default). Numbers are equivalent to Copy.
> With CycloneDDS + iceoryx, true zero-copy loaned messages would reduce
> latency to the SHM level.

### Executor callback scheduling jitter (enqueue → dispatch)

| Transport | Rate (Hz) | Callbacks | p50 (µs) | p99 (µs) | Missed deadlines |
|-----------|----------:|----------:|---------:|---------:|-----------------:|
| Copy      | 100       |     1 965 |        0 |        3 |                0 |
| Copy      | 500       |     8 192 |        0 |        8 |                0 |
| Copy      | 1000      |     8 192 |        0 |        8 |                0 |
| Loaned    | 100       |     1 966 |        0 |       11 |                0 |
| Loaned    | 500       |     8 192 |        0 |        7 |                0 |
| Loaned    | 1000      |     8 192 |        0 |        6 |                0 |
| SHM       | 100       |     1 967 |        0 |        3 |                0 |
| SHM       | 500       |     8 192 |        0 |        9 |                0 |
| SHM       | 1000      |     8 192 |        0 |       10 |                0 |

Executor jitter p99 **≤ 11 µs** with no RT scheduling (WSL, standard kernel).
On a bare-metal `PREEMPT_RT` system with `SCHED_FIFO` and CPU isolation,
expect p99 **< 5 µs**.

> **p50 = 0 µs** — jitter is reported in integer µs; most callbacks dispatched
> in under 500 ns, which rounds to 0. Raw nanosecond samples are available via
> `RTExecutor::compute_stats()`.
>
> **callbacks = 8 192** — the jitter ring buffer holds 8 192 samples; at 500 Hz
> and 1000 Hz the buffer fills before the run ends. All subsequent callbacks
> are still dispatched correctly — only the jitter recording saturates.

### Target numbers (tuned RT hardware)

- SHM, 12-element joint state @ 1000 Hz → **p99 < 50 µs**
- Copy, 12-element joint state @ 1000 Hz → **p99 < 200 µs**
- Executor jitter with `SCHED_FIFO` + isolated core → **p99 < 5 µs**

---

## Design Decisions

### Lock-free ring buffer over `std::mutex`

A mutex in the callback hot path introduces two problems: priority inversion
(a low-priority thread holding the lock stalls the RT executor) and
non-deterministic wake latency from the OS futex. The SPSC ring buffer needs
no locking because there is exactly one writer and one reader; acquire/release
ordering on the head and tail atomics is sufficient. The Capacity-1 sentinel
slot trick avoids a separate counter atomic.

### Tagged-pointer ABA prevention in `MemoryPool`

The classic lock-free freelist suffers ABA: slot A is freed, reallocated,
and freed again before a concurrent CAS sees the original head. The raw
pointer re-appears unchanged, so the CAS succeeds with a stale `next` link,
corrupting the freelist. Packing a 32-bit version counter into the upper half
of a 64-bit atomic makes every compare-exchange unique, eliminating ABA
without requiring 128-bit CMPXCHG16B.

### Cache-line padding on `head_` and `tail_`

Both producer and consumer run on separate cores. Without padding they would
share a cache line, causing false sharing: every `head_.store()` by the
producer invalidates the consumer's cached line holding `tail_`, and vice
versa. `alignas(64)` places each atomic on its own line, so each core's L1
cache stays hot independently.

### `SCHED_FIFO` over `SCHED_RR`

`SCHED_FIFO` never time-slices a running thread unless it voluntarily yields
or blocks. For a busy-wait spin loop that must not be preempted,
`SCHED_FIFO` is strictly better than `SCHED_RR`. The RT executor only yields
when the queue is empty (either by spinning or nanosleep), so starvation of
lower-priority threads is bounded by the callback execution time.

### Timestamp embedding in payload vs. a custom message type

Encoding the 64-bit `steady_clock` timestamp as a `double` in `data[0]` of
`Float64MultiArray` avoids defining a custom `.msg` file and the associated
build complexity. A `double` has 52-bit mantissa precision, which represents
~4500 years of nanoseconds without loss — adequate for benchmark durations.
For PointCloud2 the standard `header.stamp` fields are repurposed (high/low
32-bit split) since PointCloud2 naturally carries a header.

### Memory pre-allocation strategy

The `MemoryPool` allocates its entire storage array as a value member (not on
the heap) at construction time. This means the pool itself can live on the
stack or as a node member. No heap allocation occurs after the executor is
constructed — critical for avoiding `malloc` latency spikes in the RT loop.

### No `std::cout` in the hot path

`std::cout` acquires a global lock and may trigger a write syscall.
All logging is pushed into a `CsvLogger` ring buffer; a background thread
drains it to disk. The RT spin loop only touches the lock-free queue and
the memory pool.

---

## Environment Notes

- OS: Ubuntu 22.04 with `PREEMPT_RT` patch recommended (not required)
- ROS2: Humble
- RMW: `rmw_cyclonedds_cpp` (preferred for loaned message support)
  ```bash
  export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp
  ```
- Disable CPU frequency scaling for consistent results:
  ```bash
  sudo cpupower frequency-set --governor performance
  ```
- Disable address space layout randomisation (optional, tightens jitter):
  ```bash
  echo 0 | sudo tee /proc/sys/kernel/randomize_va_space
  ```
