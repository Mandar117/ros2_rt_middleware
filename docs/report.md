---
title: "Custom Real-Time ROS2 Middleware"
subtitle: "Design, Implementation, and Benchmarking"
author: "Sagar Gokhale"
date: "October 2026"
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

I designed and built a real-time middleware layer for ROS 2 Humble in C++20.
It has three parts:

- **A real-time executor** that releases periodic work on absolute deadlines.
- **Two zero-copy-capable transports:** loaned messages over fixed-size types,
  and a POSIX shared-memory channel.
- **A benchmark suite** that compares the RT executor against the stock
  `rclcpp` executor on the same workload, in-process and cross-process, idle
  and under CPU load.

The system is covered by 56 unit and integration tests and was measured over
more than 530,000 message samples.

On a WSL2 host with **no real-time scheduling available**, the RT executor
releases a 1 kHz control task with **22 µs median / 49 µs p99** wake-up
lateness, against **154 / 184 µs** for the stock `rclcpp` timer. The stock
timer also runs **9% fast** on that host because of a clock-domain mismatch
I traced inside rcl.

The shared-memory transport delivers cross-process messages in **32 µs median
/ 59 µs p99**, about 3× faster than the DDS copy path. Under CPU load it holds
**42 µs p99** while the DDS path degrades to **3.9 ms**.

---

# Motivation

A 1 kHz joint-control loop has two timing problems that the default ROS 2
stack does not address.

**Release precision.** The control step must start on time, every period.
`rclcpp` wall timers wake through the executor's wait set. Their lateness
depends on the scheduler, on timer slack, and (as it turned out) on which
kernel clock the timer is scheduled against.

**Transport latency and tail behaviour.** Every DDS message is serialised,
copied and delivered through the middleware's own receive threads. Those
threads compete for CPU with everything else on the machine, so the
latency tail is set by system load, not by the application.

This project tackles both, and measures the result against the stock stack
on identical workloads, so every claim comes with a baseline.

---

# System Architecture

The project is three layers, each depending only on the layer below:

```
  ┌───────────────────────────────────────────────────────────────┐
  │   benchmark_runner  ·  latency nodes  ·  analysis scripts     │
  ├───────────────────────────────────────────────────────────────┤
  │   RTExecutor  ·  Publisher/Subscriber  ·  ShmChannel  ·  CSV   │
  ├───────────────────────────────────────────────────────────────┤
  │   LockFreeRingBuffer  ·  MemoryPool  ·  SampleSet             │
  └───────────────────────────────────────────────────────────────┘
```

The primitive layer is header-only and has no ROS 2 dependency. The
middleware layer depends on `rclcpp` but not on any particular message type.
The application layer wires everything into measurable scenarios.

---

# Implementation

## Real-Time Executor

`RTExecutor` runs two kinds of work on one thread:

- a **periodic task** with absolute-deadline release;
- **one-shot callbacks** pushed through a lock-free queue.

**Absolute-deadline release.** The next release is computed as
`next += period`, and the thread sleeps to it with
`clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`. It can also busy-wait.
Because each release is anchored to the schedule rather than to the previous
wake-up, lateness never accumulates into drift. The executor records each
release's lateness (start minus scheduled release) as its scheduling jitter.

**Overrun handling.** If a release starts so late that later releases are
already in the past, those releases are skipped and counted as missed. I
matched the rule `rcl_timer_call` uses, so missed-deadline counts are
directly comparable with the stock timer.

**Thread configuration.** At the start of `spin()`, the executor:

- pins the thread to a core (`pthread_setaffinity_np`);
- applies `SCHED_FIFO` or `SCHED_RR`;
- reduces timer slack to 1 ns (`PR_SET_TIMERSLACK`);
- optionally locks memory (`mlockall`).

It records which of these actually succeeded, so benchmark results always
state the conditions they were measured under.

**Timer slack** turned out to matter a lot. `SCHED_OTHER` threads inherit
50 µs of slack, so the kernel may defer every wake-up by that much to batch
timers. RT policies get zero slack automatically, but when `SCHED_FIFO` is
unavailable the thread silently keeps the default. Setting slack to 1 ns
alone reduced median wake-up lateness from about 85 µs to about 20 µs on
the test machine.

**One-shot queue.** Callbacks travel through an SPSC ring buffer of pointers
to wrappers taken from a pre-allocated memory pool, so the dispatch path
neither locks nor allocates. `enqueue()` is safe from any thread: producers
serialise on a spinlock that the dispatch thread never touches, so a
real-time consumer can never be blocked by a producer.

## Lock-Free Ring Buffer and Memory Pool

**Ring buffer.** The ring uses power-of-two capacity with bitmask indexing,
and one sentinel slot to tell a full buffer from an empty one. Head and tail
are cache-line-padded so producer and consumer don't falsely share a line.
Acquire/release ordering is the minimum synchronisation SPSC needs.

**Memory pool.** The pool's freelist is lock-free and protected against the
ABA problem. Its head packs a 32-bit version and a 32-bit slot index into
one 64-bit atomic, so a recycled slot can never satisfy a stale
compare-and-swap. This works without a 128-bit CAS (`CMPXCHG16B`), which
some ARM targets lack. The `next` links live in a parallel array, so freelist
metadata never aliases live objects.

## Message Types

I replaced the original `Float64MultiArray` (which carried a timestamp
bit-packed into a `double`) with two fixed-size messages:

- `JointSample`: `publish_ns`, `seq`, and 12 `float64` positions (96 B).
- `CloudSample`: `publish_ns`, `seq`, width, point step, and a 3840-byte
  point buffer.

Fixed size is the precondition for zero-copy loans: an RMW can only lend
memory for a type whose size is known at compile time, so an unbounded
sequence can never be loaned on any middleware. The explicit `seq` field
also enables loss detection.

## Transports

**Copy.** A standard `publish(const&)`; the RMW serialises and copies.

**Loaned.** The publisher calls `borrow_loaned_message()` and fills the
payload in place, but only when `can_loan_messages()` confirms the RMW really
loans memory. Otherwise it falls back to Copy and says so. The subscriber
uses `const&` callbacks so the executor can hand it loaned memory directly.

**SHM.** A shared-memory ring that bypasses DDS entirely, both for data and
for wake-up:

- **Writes.** The writer fills a slot under a per-slot seqlock: the slot's
  sequence is odd while it is being written, then set to the sample's even
  sequence. The writer then advances a shared write index.
- **Wake-up.** The writer bumps a process-shared futex word, and issues a
  `FUTEX_WAKE` syscall only if a reader has announced that it is sleeping.
- **Reads.** A reader copies the slot out and re-checks the sequence. If the
  writer lapped it, or overwrote the slot mid-copy, the sample is counted as
  an overrun instead of being returned torn.

The writer never waits for readers, so a slow subscriber cannot stall a
control loop. A 100,000-sample concurrent stress test checks that no torn
sample is ever delivered and that every sample is either delivered or
counted.

## Measurement Methodology

- **One-way latency.** Latency is the subscriber's `steady_clock` time at the
  first line of the receive path, minus the publisher's stamp. Both sides use
  `CLOCK_MONOTONIC` on the same host.
- **Wake-up lateness.** For the RT executor, the executor records it itself.
  For the stock timer, the publisher measures it against rcl's own schedule,
  read back with `time_until_trigger()` on rcl's steady clock.
- **Subscriber.** The subscriber blocks in its wait set (DDS modes) or on
  the futex (SHM). It never sleep-polls.
- **Accounting.** Each run reports:
  - the achieved publish rate;
  - sequence-gap loss and logger drops;
  - whether loans were really granted;
  - which RT settings took effect;
  - the kernel, RMW, rlimits, and the measured skew between
    `CLOCK_MONOTONIC_RAW` and `CLOCK_MONOTONIC`.
- **Warm-up.** The first second of every run is discarded.
- **Load.** The load generator runs threads that stream through 8 MB buffers
  (evicting caches) and churn the heap allocator.

---

# Test Suite

| Binary | Cases | What is tested |
|---|--:|---|
| `test_ring_buffer` | 10 | push/pop, FIFO, wrap-around, full/empty guards, move-only types, SPSC 100k-item concurrent run |
| `test_memory_pool` | 9 | availability, exhaustion, recycling, pointer uniqueness, 4-thread × 10k stress |
| `test_executor_ordering` | 20 | FIFO dispatch, stop-before-spin, re-spin, 4-producer enqueue, destructor cleanup, sample cap, CSV export, periodic rate, overrun accounting, queue/periodic interleave, SCHED_RR applied, timer slack |
| `test_shm_channel` | 10 | round trip, backlog skip, ordering, lapped-reader overruns, futex wake-up, timeout, writer close, replacement safety, torn-read stress |
| `test_latency_end_to_end` | 7 | CSV schema; Copy, Loaned and SHM delivery of both message types; sequence ordering; stock-timer and RT-executor publishing |

All 56 tests pass on WSL2 / Ubuntu 22.04 / ROS 2 Humble. They also passed
20 repeated runs idle and 5 under eight busy threads, with no flakes. GitHub
Actions builds the package, runs the suite, and performs a short benchmark
run, whose summary it publishes with every push.

---

# Results

**Environment:** WSL2, Ubuntu 22.04, kernel 6.18 (no `PREEMPT_RT`), 4 vCPU,
ROS 2 Humble, Fast DDS. WSL2 grants no RT priority (`rtprio = 0`), so the RT
executor ran under `SCHED_OTHER`. Fast DDS did not grant loans for these
types, so Loaned mode fell back to copying. Each scenario ran for 10 s.

## Publisher Wake-Up Lateness

In-process, idle, all transports pooled:

| Rate | Stock p50 | Stock p99 | RT p50 | RT p99 | Stock achieved rate |
|---:|---:|---:|---:|---:|---:|
| 100 Hz  | 897 µs | 1036 µs | 32 µs | 101 µs | 109.0 Hz |
| 500 Hz  | 238 µs | 286 µs  | 19 µs | 56 µs  | 545.4 Hz |
| 1000 Hz | 154 µs | 184 µs  | 22 µs | 49 µs  | 1090.9 Hz |

The stock timer's lateness is about 9% of its period at every rate, and it
fires about 9% too often. I traced this to rcl's clocks:

- rcl schedules timers on `CLOCK_MONOTONIC_RAW`, while the kernel wait that
  implements the timeout runs on `CLOCK_MONOTONIC`.
- On this host, the raw clock ran 9.09% faster (measured: 90,906 ppm), so
  every wait overshoots by the skew and the schedule itself runs fast.
- The RT executor schedules and sleeps on the same clock, so it is immune.

On bare metal the skew is a few ppm; the stock numbers would then reduce to
their scheduling and timer-slack component.

## One-Way Message Latency

Cross-process, idle, RT executor, 96-byte joint samples:

| Transport | 100 Hz p50/p99 | 500 Hz p50/p99 | 1000 Hz p50/p99 |
|---|---:|---:|---:|
| Copy   | 194 / 301 µs | 110 / 194 µs | 96 / 158 µs |
| Loaned (copy fallback) | 164 / 285 µs | 109 / 195 µs | 97 / 155 µs |
| SHM    | 49 / 82 µs   | 34 / 61 µs   | 32 / 59 µs  |

Message latency is the same under the stock and RT executors: the executor
decides when a sample is published, not how fast the transport delivers it.
Latency is higher at 100 Hz because the idle subscriber sleeps more deeply
between messages. The second message of each cycle (the cloud sample) is
consistently faster than the first, because the joint sample pays the cost
of waking the subscriber thread.

## Under CPU Load

In-process, RT executor, 4 load threads on 4 vCPUs, 1000 Hz:

| Transport | p50 | p99 | p99.9 |
|---|---:|---:|---:|
| Copy | 76 µs | 3932 µs | 5276 µs |
| Loaned (copy fallback) | 73 µs | 3204 µs | 5019 µs |
| SHM | 8 µs | 42 µs | 96 µs |

The DDS paths' tails grow by about 40×, because Fast DDS delivers through its
own receive threads, which compete with the load. The SHM reader is woken
directly by the publisher's futex and is barely affected.

The executor's own wake-up p99 also degrades under load, to 1.9 ms at 1 kHz.
That is the expected result for a `SCHED_OTHER` thread, and it is precisely
what `SCHED_FIFO` on an isolated core exists to prevent. Measuring that is
the next step.

---

# Challenges and Corrections

**The first benchmark measured its own sleep.** The original runner polled
the subscriber with a 200 µs sleep between `spin_some` calls, which added up
to 200 µs to every sample. That polling dominated the 90–150 µs medians I
first reported. The subscriber now blocks in its wait set.

**The first "SHM" results were not SHM.** In the version that produced the
original numbers, SHM mode was a stub that published through DDS like Copy
mode. A later version wrote the payload to shared memory but still woke the
subscriber with a DDS message, so it paid the full middleware latency anyway.
The current channel does both data and wake-up in shared memory.

**`Float64MultiArray` could never be loaned.** Loaned messages require a
fixed-size type. Switching to custom fixed-size messages was the only way to
make the Loaned mode meaningful.

**Executor defects.**
- `SCHED_RR` could never be selected, because of an early return.
- `enqueue()` was documented as thread-safe, but the queue only supported
  one producer.
- A `stop()` issued before `spin()` started could be lost.
- Queued callbacks were never destroyed when the executor was.
- The jitter statistic stopped counting at its 8192-sample buffer.

Each of these is fixed and covered by a test.

**An unexpected clock bug.** The first stock-vs-RT comparison showed the
stock timer with hugely *negative* lateness and too many publishes. I
ruled out my own accounting, then measured `CLOCK_MONOTONIC_RAW` against
`CLOCK_MONOTONIC`: the two drifted 5–9% apart. That traced the cause to
rcl's choice of clock. The benchmark now measures stock-timer lateness on
rcl's own clock and records the skew in every run.

**Linker conflicts from `main()` colocation.** Class implementations
originally lived in the same files as the node `main()` functions. I moved
them into a `latency_transport` static library, and the node files are now
thin wrappers.

---

# Conclusion

The project demonstrates measurable gains on hardware that offers no
real-time support at all:

- **Release precision.** Wake-up lateness is about 7× lower at 1 kHz, and
  the RT executor holds the requested rate exactly while the stock timer runs
  9% fast.
- **Transport latency.** The SHM channel is about 3× faster than DDS
  cross-process, and keeps a sub-100 µs tail under load where DDS reaches
  milliseconds.

Every number comes with its baseline, its environment, and its accounting
for loss.

The remaining step is to run the same matrix where the design's other half
can take effect: a `PREEMPT_RT` kernel, an isolated core, `SCHED_FIFO`, and
CycloneDDS with iceoryx for true zero-copy loans. The benchmark records those
conditions automatically, so the results will be directly comparable.
