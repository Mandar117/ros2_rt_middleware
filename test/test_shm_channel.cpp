#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include "shm_channel.hpp"

using rt_middleware::kShmMaxPayload;
using rt_middleware::kShmSlotCount;
using rt_middleware::ShmChannelReader;
using rt_middleware::ShmChannelWriter;
using rt_middleware::ShmSample;
using Result = ShmChannelReader::Result;
using namespace std::chrono_literals;

namespace {

// Unique per test and per process so parallel ctest runs never collide.
std::string channel_name() {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  return "/rtmw_test_" + std::string(info->name()) + "_" + std::to_string(getpid());
}

}  // namespace

TEST(ShmChannel, ReaderCannotOpenMissingChannel) {
  const std::string name = channel_name();
  shm_unlink(name.c_str());
  ShmChannelReader reader(name);
  EXPECT_FALSE(reader.try_open());
  ShmSample s;
  EXPECT_EQ(reader.read(s, 1ms), Result::NotOpen);
}

TEST(ShmChannel, RoundTripSingleSample) {
  const std::string name = channel_name();
  ShmChannelWriter writer(name);
  ShmChannelReader reader(name);
  ASSERT_TRUE(reader.try_open());

  const double payload[3] = {1.5, -2.25, 3.0};
  writer.write(7, 123456789, 42, payload, sizeof(payload));

  ShmSample s;
  ASSERT_EQ(reader.read(s, 100ms), Result::Ok);
  EXPECT_EQ(s.type, 7u);
  EXPECT_EQ(s.publish_ns, 123456789);
  EXPECT_EQ(s.seq, 42u);
  ASSERT_EQ(s.size, sizeof(payload));
  EXPECT_EQ(std::memcmp(s.payload, payload, sizeof(payload)), 0);
}

TEST(ShmChannel, ReaderSkipsBacklogPublishedBeforeOpen) {
  const std::string name = channel_name();
  ShmChannelWriter writer(name);
  const int v = 1;
  writer.write(0, 0, 0, &v, sizeof(v));

  ShmChannelReader reader(name);
  ASSERT_TRUE(reader.try_open());
  ShmSample s;
  EXPECT_EQ(reader.read(s, 10ms), Result::Timeout);

  writer.write(0, 0, 1, &v, sizeof(v));
  ASSERT_EQ(reader.read(s, 100ms), Result::Ok);
  EXPECT_EQ(s.seq, 1u);
}

TEST(ShmChannel, PreservesOrderWithinRingDepth) {
  const std::string name = channel_name();
  ShmChannelWriter writer(name);
  ShmChannelReader reader(name);
  ASSERT_TRUE(reader.try_open());

  const uint64_t n = kShmSlotCount - 1;
  for (uint64_t i = 0; i < n; ++i) {
    writer.write(0, static_cast<int64_t>(i), i, &i, sizeof(i));
  }
  ShmSample s;
  for (uint64_t i = 0; i < n; ++i) {
    ASSERT_EQ(reader.read(s, 100ms), Result::Ok);
    EXPECT_EQ(s.seq, i);
  }
  EXPECT_EQ(reader.overruns(), 0u);
  EXPECT_EQ(reader.read(s, 1ms), Result::Timeout);
}

TEST(ShmChannel, LappedReaderCountsOverruns) {
  const std::string name = channel_name();
  ShmChannelWriter writer(name);
  ShmChannelReader reader(name);
  ASSERT_TRUE(reader.try_open());

  const uint64_t n = 200;
  for (uint64_t i = 0; i < n; ++i) {
    writer.write(0, 0, i, &i, sizeof(i));
  }

  ShmSample s;
  uint64_t received = 0;
  uint64_t first_seq = 0;
  while (reader.read(s, 1ms) == Result::Ok) {
    if (received == 0) first_seq = s.seq;
    ++received;
  }
  EXPECT_EQ(received, kShmSlotCount);
  EXPECT_EQ(first_seq, n - kShmSlotCount);
  EXPECT_EQ(reader.overruns(), n - kShmSlotCount);
}

TEST(ShmChannel, BlockedReaderWakesOnWrite) {
  const std::string name = channel_name();
  ShmChannelWriter writer(name);
  ShmChannelReader reader(name);
  ASSERT_TRUE(reader.try_open());

  std::atomic<int> result{-1};
  std::atomic<int64_t> woke_after_ns{0};
  std::chrono::steady_clock::time_point written_at;

  std::thread t([&] {
    ShmSample s;
    result.store(static_cast<int>(reader.read(s, 2s)));
    woke_after_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - written_at).count());
  });

  std::this_thread::sleep_for(50ms);  // let the reader block on the futex
  const int v = 5;
  written_at = std::chrono::steady_clock::now();
  writer.write(0, 0, 0, &v, sizeof(v));
  t.join();

  EXPECT_EQ(result.load(), static_cast<int>(Result::Ok));
  EXPECT_LT(woke_after_ns.load(), 500'000'000) << "futex wake took too long";
}

TEST(ShmChannel, ReadTimesOutWhenIdle) {
  const std::string name = channel_name();
  ShmChannelWriter writer(name);
  ShmChannelReader reader(name);
  ASSERT_TRUE(reader.try_open());

  ShmSample s;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(reader.read(s, 30ms), Result::Timeout);
  EXPECT_GE(std::chrono::steady_clock::now() - start, 30ms);
}

TEST(ShmChannel, ReaderSeesClosedWhenWriterGoesAway) {
  const std::string name = channel_name();
  auto writer = std::make_unique<ShmChannelWriter>(name);
  ShmChannelReader reader(name);
  ASSERT_TRUE(reader.try_open());

  std::atomic<int> result{-1};
  std::thread t([&] {
    ShmSample s;
    result.store(static_cast<int>(reader.read(s, 2s)));
  });
  std::this_thread::sleep_for(50ms);
  writer.reset();
  t.join();

  EXPECT_EQ(result.load(), static_cast<int>(Result::Closed));
  // Region is unlinked: a fresh reader cannot open it.
  ShmChannelReader late(name);
  EXPECT_FALSE(late.try_open());
}

TEST(ShmChannel, OldWriterDoesNotUnlinkReplacement) {
  const std::string name = channel_name();
  auto first  = std::make_unique<ShmChannelWriter>(name);
  auto second = std::make_unique<ShmChannelWriter>(name);  // replaces the name
  first.reset();

  ShmChannelReader reader(name);
  ASSERT_TRUE(reader.try_open()) << "first writer unlinked the second's region";
  const int v = 9;
  second->write(0, 0, 3, &v, sizeof(v));
  ShmSample s;
  ASSERT_EQ(reader.read(s, 100ms), Result::Ok);
  EXPECT_EQ(s.seq, 3u);
}

TEST(ShmChannel, ConcurrentStressNeverReturnsTornData) {
  const std::string name = channel_name();
  ShmChannelWriter writer(name);
  ShmChannelReader reader(name);
  ASSERT_TRUE(reader.try_open());

  static constexpr uint64_t kSamples = 100'000;
  // Payload size and every byte are a function of seq, so a torn read (bytes
  // from two different samples) is detectable.
  auto size_for = [](uint64_t seq) {
    return static_cast<uint32_t>(8 + (seq * 37) % (kShmMaxPayload - 8));
  };

  std::atomic<bool> writer_done{false};
  std::thread producer([&] {
    std::vector<std::byte> buf(kShmMaxPayload);
    for (uint64_t seq = 0; seq < kSamples; ++seq) {
      const uint32_t size = size_for(seq);
      std::memset(buf.data(), static_cast<int>(seq & 0xFF), size);
      writer.write(1, static_cast<int64_t>(seq), seq, buf.data(), size);
    }
    writer_done.store(true);
  });

  ShmSample s;
  uint64_t received = 0;
  uint64_t last_seq = 0;
  bool first = true;
  uint64_t torn = 0;
  while (true) {
    const Result r = reader.read(s, 20ms);
    if (r == Result::Timeout) {
      if (writer_done.load()) break;
      continue;
    }
    ASSERT_EQ(r, Result::Ok);
    if (!first) {
      ASSERT_GT(s.seq, last_seq) << "samples delivered out of order";
    }
    first = false;
    last_seq = s.seq;
    ++received;

    if (s.size != size_for(s.seq) || s.publish_ns != static_cast<int64_t>(s.seq)) {
      ++torn;
      continue;
    }
    const auto expect = static_cast<std::byte>(s.seq & 0xFF);
    for (uint32_t i = 0; i < s.size; ++i) {
      if (s.payload[i] != expect) { ++torn; break; }
    }
  }
  producer.join();

  EXPECT_EQ(torn, 0u) << "seqlock let a torn sample through";
  EXPECT_EQ(received + reader.overruns(), kSamples)
      << "every sample must be either delivered or counted as an overrun";
  EXPECT_GT(received, 0u);
}
