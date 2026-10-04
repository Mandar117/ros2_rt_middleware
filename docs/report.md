---
title: "Custom Real-Time ROS2 Middleware"
subtitle: "Design, Implementation, and Benchmarking"
author: "Sagar Gokhale"
date: "May 2026"
geometry: "top=1in, bottom=1in, left=1.1in, right=1.1in"
fontsize: 11pt
linestretch: 1.35
colorlinks: true
linkcolor: NavyBlue
urlcolor: NavyBlue
header-includes:
  - \usepackage{booktabs}
  - \usepackage{xcolor}
  - \usepackage{fancyhdr}
  - \pagestyle{fancy}
  - \fancyhf{}
  - \fancyhead[L]{\small Custom ROS2 RT Middleware}
  - \fancyhead[R]{\small Sagar Gokhale}
  - \fancyfoot[C]{\thepage}
  - \definecolor{codegray}{gray}{0.95}
---

# Overview

I designed and implemented a custom real-time middleware layer for ROS2 Humble in C++20 that addresses a fundamental limitation of the default ROS2 executor: non-deterministic callback dispatch latency. The project comprises three tightly composed layers — a lock-free SPSC ring buffer, an ABA-safe fixed-size memory pool, and a POSIX real-time executor — along with a complete latency benchmarking suite measuring one-way message transport across three transport modes at publish rates up to 1 kHz. The system was validated with 29 unit tests and characterized over 95,951 message samples, achieving executor scheduling jitter of p99 ≤ 11 µs on a standard kernel with zero missed deadlines across all scenarios.

---

# Motivation

The default `rclcpp::SingleThreadedExecutor` uses `std::mutex`-protected callback queues designed for correctness and generality, not determinism. This creates two failure modes for time-critical robotic control loops:

**Priority inversion.** A low-priority thread holding the queue mutex stalls a higher-priority RT thread waiting to dequeue, causing unbounded blocking at the OS futex level.

**Non-deterministic wake latency.** `std::mutex` under contention involves kernel futex calls whose wake delay is governed by the OS scheduler, not the application. Under load, this adds hundreds of microseconds of variance to callback dispatch.

For a joint-state control loop at 1 kHz — where a missed deadline means a servo receives its position command after its update window — these are not theoretical concerns. I built this project to demonstrate that both can be eliminated with standard POSIX primitives within the ROS2 node model, and to produce a quantified, reproducible result to back that claim.

---

# System Architecture

I organized the project as three layers with strict downward-only dependencies:

```
  ┌──────────────────────────────────────────────────────┐
  │        benchmark_runner  /  latency nodes            │
  ├──────────────────────────────────────────────────────┤
  │    RTExecutor  ·  CsvLogger  ·  Pub/Sub Transport    │
  ├──────────────────────────────────────────────────────┤
  │    LockFreeRingBuffer<T,N>  ·  MemoryPool<T,N>       │
  └──────────────────────────────────────────────────────┘
```

The **primitive layer** is header-only with zero ROS2 dependency — it can be used in any C++20 codebase. The **middleware layer** depends on `rclcpp` but is message-type agnostic. The **application layer** wires everything together into measurable scenarios. This separation means each layer can be unit-tested in isolation and replaced independently.

---

# Implementation

## Lock-Free Ring Buffer

I implemented a single-producer, single-consumer (SPSC) ring buffer as the backbone of both the callback dispatch queue and the CSV logging pipeline. Four design decisions drive its performance:

**Power-of-2 capacity with bitmask indexing** replaces modulo division with a single bitwise AND, eliminating a division instruction from the inner poll loop. Capacity is enforced at compile time via `static_assert((N & (N-1)) == 0)`.

**Capacity-1 sentinel slot** distinguishes a full buffer from an empty one without a third atomic counter. `head == tail` means empty; `(head + 1) & mask == tail` means full. This saves one acquire/release pair per operation.

**Cache-line padding on `head_` and `tail_`** via `alignas(64)` places each atomic on its own 64-byte cache line. Without this, the producer's writes to `head_` invalidate the consumer's cached line containing `tail_` — false sharing — causing L1 misses on every push and pop.

**Acquire/release memory ordering** — `memory_order_release` on stores and `memory_order_acquire` on loads — provides the minimum synchronization needed for SPSC correctness without the full sequential-consistency fence of `memory_order_seq_cst`.

## Memory Pool

I pre-allocate 512 callback wrapper slots at executor construction time, eliminating `malloc` calls from the RT hot path. The pool's freelist is lock-free but susceptible to the ABA problem: if slot S is freed, reallocated, and freed again before a concurrent CAS reads it, the stale pointer compares equal and the CAS succeeds with a corrupted `next` link.

I solve this with a **tagged-pointer freelist**: I pack a 32-bit version counter into the upper 32 bits of a 64-bit atomic, and the slot index into the lower 32 bits. Every deallocation increments the version, making every CAS unique even when the same slot is recycled. Crucially, `next` pointers live in a parallel `next_[]` array — not in the object storage itself — so there is no aliasing between live callback data and freelist metadata. This eliminates ABA without requiring 128-bit `CMPXCHG16B`, which is unavailable on some ARM targets.

## Real-Time Executor

`RTExecutor` is a drop-in for `rclcpp::SingleThreadedExecutor`. On `spin()`, it optionally applies two POSIX thread configurations:

**CPU affinity** via `pthread_setaffinity_np` pins the RT thread to a dedicated core, preventing OS-scheduler-driven core migrations and keeping the L1/L2 cache warm across callback invocations. On a system with `isolcpus` kernel boot parameters, this is the highest single-impact RT tuning available.

**SCHED_FIFO scheduling** via `pthread_setschedparam` prevents time-slicing of the RT thread. Unlike `SCHED_RR`, `SCHED_FIFO` never preempts a running thread without an explicit yield or block — the RT thread holds the CPU for the full duration of each callback. I chose `SCHED_FIFO` over `SCHED_RR` specifically because the busy-wait spin path never voluntarily yields, and `SCHED_RR` would preempt it at the time-slice boundary.

The dispatch hot path is allocation-free: the feeder thread acquires a pre-allocated wrapper from the `MemoryPool` via placement-new and pushes it to the SPSC queue; the RT thread pops it, records the enqueue timestamp, executes the callback, records the dispatch timestamp, destroys the wrapper in-place, and returns the slot to the pool. Per-callback jitter (dispatch time minus enqueue time) is pushed into a secondary 8192-element ring buffer for post-run percentile analysis.

Two idle modes are supported: **busy-wait** (`atomic_thread_fence(seq_cst)` on each empty poll — zero response latency at 100% idle CPU) and **nanosleep** (yields for a configurable duration, trading ~2–10 µs of added latency for near-zero idle CPU).

## Transport & Benchmarking

I measure **one-way latency**: the publisher embeds `steady_clock::now()` as nanoseconds into the message at publish time, and the subscriber reads `steady_clock::now()` at the first line of its callback. The difference is the measured latency. Both nodes run on the same host, sharing the same monotonic clock — no clock synchronisation is required.

For `Float64MultiArray` (12 doubles, 96 bytes — simulating a joint state vector), I encode the 64-bit nanosecond timestamp as a `double` in `data[0]` using `memcpy` rather than a type cast, avoiding undefined behaviour from type-punning. A `double` has 52-bit mantissa precision, representing ~4,500 years of nanoseconds without loss. For `PointCloud2` (320×1 xyz point cloud), I split the timestamp across the standard `header.stamp.sec` and `header.stamp.nanosec` fields using a 32-bit high/low encoding.

I benchmarked three transport modes:

- **Copy** — standard `publish(msg)`. The RMW serializes and copies the buffer through the DDS layer.
- **Loaned** — `borrow_loaned_message()` API, intended for zero-copy when the RMW supports it. FastDDS (the default RMW) falls back to local allocation; true zero-copy requires CycloneDDS + iceoryx.
- **SHM** — POSIX shared-memory fallback. The publisher writes directly into a shared segment; the subscriber reads without a kernel-mediated copy.

The benchmark runner spawns three threads per scenario: an RT thread running `exec.spin()` (SPSC consumer), a feeder thread pushing `rclcpp::spin_some(pub_node)` at 2× the publish rate (SPSC producer), and a subscriber thread polling `rclcpp::spin_some(sub_node)` every 200 µs. This correctly separates the SPSC producer and consumer onto distinct threads, which an earlier single-thread design failed to do — that bug produced only 2 data points per scenario instead of thousands.

---

# Test Suite

I wrote 29 unit tests across three Google Test binaries:

| Binary | Cases | What is tested |
|---|---|---|
| `test_ring_buffer` | 10 | Empty/push/pop, FIFO ordering, wrap-around, full/empty guards, move-only types, SPSC 100k-item concurrent throughput |
| `test_memory_pool` | 10 | Availability tracking, exhaustion, recycling, pointer uniqueness, concurrent stress (4 threads × 10k ops) |
| `test_executor_ordering` | 9 | Single callback, FIFO ordering, stop signal, back-pressure, jitter stats correctness |

All 29 tests pass on WSL2 Ubuntu 22.04 with ROS2 Humble. A GitHub Actions CI workflow (`ubuntu-22.04`, `ros-tooling/setup-ros@v0.7`) runs the full suite automatically on every push.

---

# Results

Benchmarks were run on WSL2 / Ubuntu 22.04 / ROS2 Humble (Windows 11 host, kernel 6.6.114). SCHED_FIFO was unavailable (WSL lacks `CAP_SYS_NICE`). Numbers reflect standard kernel scheduling.

## One-Way Message Latency

| Transport | Rate (Hz) | Samples | p50 (µs) | p99 (µs) | Max (µs) |
|-----------|----------:|--------:|---------:|---------:|---------:|
| Copy      | 100       | 1,998   | 130.4    | 345.4    | 432.4    |
| Copy      | 500       | 10,020  | 153.9    | 325.8    | 976.2    |
| Copy      | 1000      | 19,948  | 93.7     | 320.4    | 379.4    |
| Loaned    | 100       | 2,002   | 143.3    | 363.0    | 494.2    |
| Loaned    | 500       | 10,002  | 154.7    | 324.5    | 450.4    |
| Loaned    | 1000      | 19,987  | 97.3     | 322.5    | 417.4    |
| SHM       | 100       | 2,002   | 148.9    | 350.2    | 436.9    |
| SHM       | 500       | 9,980   | 151.5    | 327.8    | 487.5    |
| SHM       | 1000      | 20,012  | 90.8     | 320.7    | 556.7    |

The latency values are dominated by WSL2's virtualized IPC layer, not the middleware itself. Loaned-message numbers are identical to Copy because FastDDS does not implement the zero-copy loaned API — it silently falls back to local allocation. On bare metal with CycloneDDS + iceoryx, the SHM path would show materially lower latency.

## Executor Scheduling Jitter

| Rate (Hz) | Callbacks recorded | p50 (µs) | p99 (µs) | Missed deadlines |
|----------:|-------------------:|---------:|---------:|-----------------:|
| 100       | 1,966              | < 1      | 3        | 0                |
| 500       | 8,192 (buffer cap) | < 1      | 9        | 0                |
| 1000      | 8,192 (buffer cap) | < 1      | 9        | 0                |

p50 < 1 µs means the median callback was dispatched in under 500 ns (reported as 0 in integer µs). p99 ≤ 11 µs across all scenarios with no real-time scheduling and zero missed deadlines. On a bare-metal PREEMPT_RT kernel with an isolated core and SCHED_FIFO priority 80, I expect p99 to fall below 5 µs.

---

# Challenges

**SPSC producer/consumer thread confusion.** The initial benchmark implementation called both `exec.enqueue()` and `exec.spin()` from the same thread. Since `spin()` blocks consuming callbacks until the queue empties, and no further enqueues were issued afterward, each 10-second run produced exactly 2 data points. The fix was to separate the feeder (enqueuer) and the executor (consumer) onto distinct threads, restoring the correct SPSC contract.

**Linker conflicts from `main()` colocation.** The original design placed `CsvLogger`, `LatencyPublisher`, and `LatencySubscriber` implementations in the same translation units as their `main()` functions. `benchmark_runner` needed those implementations but could not link against files containing competing `main` symbols. I resolved this by extracting all class implementations into a `latency_impl.cpp` static library (`latency_transport`), reducing the node files to thin `main()` wrappers.

**Compile error from type mismatch.** An earlier version of `publish_joint_state()` called `pack_ns_into_header()` — which takes a `std_msgs::msg::Header&` — with `joint_msg_.layout.data_offset`, a `uint32_t`. The actual timestamp was already encoded in `data[0]` via `memcpy` two lines later, making this call both type-incorrect and redundant. Removing it resolved the compile error.

---

# Conclusion

I built a real-time ROS2 middleware layer from first principles, with deliberate and defensible choices at every level: SPSC over mutex (eliminates lock contention and priority inversion), tagged-pointer ABA prevention over 128-bit CAS (portable to ARM), `SCHED_FIFO` over `SCHED_RR` (no time-slice interruption in busy-wait path), `memcpy` timestamp embedding over custom messages (zero build complexity), background CSV writer over synchronous I/O (keeps the RT thread allocation-free). The result is a system with sub-microsecond median callback jitter and p99 under 11 µs on a standard kernel — a quantified, reproducible answer to what real-time middleware engineering looks like in practice.
