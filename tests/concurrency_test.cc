#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "hotblock/block_cache.h"
#include "test_util.h"

namespace hotblock {
namespace {

using testing_util::FaultyDevice;
using testing_util::MakeBlock;
using testing_util::PeekBlock;

constexpr std::size_t kBlockSize = 512;

struct Config {
  std::string name;
  std::size_t capacity;
  std::size_t shards;
  bool background_flush;
};

// Every suite runs against: a cache that holds the whole working set, one under
// heavy eviction pressure, and a pressured one with the flusher racing writers.
class ConcurrencyTest : public ::testing::TestWithParam<Config> {
 protected:
  CacheOptions MakeOptions() const {
    CacheOptions o;
    o.capacity_blocks = GetParam().capacity;
    o.num_shards = GetParam().shards;
    o.background_flush = GetParam().background_flush;
    o.flush_interval = std::chrono::milliseconds(1);
    o.flush_dirty_ratio = 0.1;
    return o;
  }

  // Pin with retry: under extreme pressure all frames of a shard may be
  // momentarily pinned by other threads.
  static void PinRetry(BlockCache& cache, BlockId id, BlockHandle* h) {
    for (;;) {
      Status s = cache.Pin(id, h);
      if (s.ok()) return;
      ASSERT_EQ(s.code(), Status::Code::kNoSpace) << s.ToString();
      std::this_thread::yield();
    }
  }
};

// Cache coherence: concurrent read-modify-write increments on a handful of hot
// blocks must never lose an update, even when the blocks are evicted, written
// back and reloaded between increments.
TEST_P(ConcurrencyTest, ReadModifyWriteNeverLosesUpdates) {
  constexpr int kThreads = 8;
  constexpr int kOpsPerThread = 5000;
  constexpr BlockId kHotBlocks = 6;
  MemoryBlockDevice dev(kBlockSize, 64);
  {
    BlockCache cache(&dev, MakeOptions());
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&, t] {
        std::mt19937_64 rng(static_cast<std::uint64_t>(t) + 1);
        for (int i = 0; i < kOpsPerThread; ++i) {
          // Mix hot blocks with cold ones to force evictions of hot blocks.
          BlockId id = (i % 3 == 0) ? kHotBlocks + rng() % 50 : rng() % kHotBlocks;
          BlockHandle h;
          PinRetry(cache, id, &h);
          if (id >= kHotBlocks) continue;
          auto lock = h.WriteLock();
          std::uint64_t counter;
          std::memcpy(&counter, h.data(), sizeof(counter));
          ++counter;
          std::memcpy(h.mutable_data(), &counter, sizeof(counter));
          h.MarkDirty();
        }
      });
    }
    for (auto& th : threads) th.join();

    std::uint64_t total = 0;
    for (BlockId id = 0; id < kHotBlocks; ++id) {
      std::uint64_t c = 0;
      ASSERT_TRUE(cache.Read(id, 0, &c, sizeof(c)).ok());
      total += c;
    }
    std::uint64_t expected = 0;
    for (int i = 0; i < kOpsPerThread; ++i) expected += (i % 3 == 0) ? 0 : 1;
    expected *= kThreads;
    EXPECT_EQ(total, expected);
    ASSERT_TRUE(cache.FlushAll().ok());
    EXPECT_EQ(cache.stats().dirty_blocks, 0u);
  }

  // The device must agree with the cache after the final flush.
  std::uint64_t device_total = 0;
  for (BlockId id = 0; id < kHotBlocks; ++id) {
    std::uint64_t c;
    std::memcpy(&c, PeekBlock(dev, id).data(), sizeof(c));
    device_total += c;
  }
  std::uint64_t expected = 0;
  for (int i = 0; i < kOpsPerThread; ++i) expected += (i % 3 == 0) ? 0 : 1;
  EXPECT_EQ(device_total, expected * kThreads);
}

// Readers holding the shared lock must always see a block written atomically
// by a single writer: every byte equal.
TEST_P(ConcurrencyTest, ReadersNeverObserveTornBlocks) {
  constexpr int kWriters = 3;
  constexpr int kReaders = 5;
  constexpr BlockId kBlocks = 24;
  MemoryBlockDevice dev(kBlockSize, kBlocks);
  BlockCache cache(&dev, MakeOptions());
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> torn{0};
  std::atomic<std::uint64_t> reads{0};

  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w] {
      std::mt19937 rng(100 + w);
      std::vector<std::uint8_t> buf(kBlockSize);
      while (!stop.load(std::memory_order_relaxed)) {
        BlockId id = rng() % kBlocks;
        std::memset(buf.data(), static_cast<int>(rng() & 0xff), buf.size());
        Status s = cache.Write(id, 0, buf.data(), buf.size());
        if (!s.ok()) ASSERT_EQ(s.code(), Status::Code::kNoSpace);
      }
    });
  }
  for (int r = 0; r < kReaders; ++r) {
    threads.emplace_back([&, r] {
      std::mt19937 rng(200 + r);
      std::vector<std::uint8_t> buf(kBlockSize);
      while (!stop.load(std::memory_order_relaxed)) {
        BlockId id = rng() % kBlocks;
        Status s = cache.Read(id, 0, buf.data(), buf.size());
        if (!s.ok()) {
          ASSERT_EQ(s.code(), Status::Code::kNoSpace);
          continue;
        }
        reads.fetch_add(1, std::memory_order_relaxed);
        for (std::size_t i = 1; i < buf.size(); ++i) {
          if (buf[i] != buf[0]) {
            torn.fetch_add(1);
            break;
          }
        }
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  stop.store(true);
  for (auto& th : threads) th.join();

  EXPECT_EQ(torn.load(), 0u);
  EXPECT_GT(reads.load(), 0u);
}

// Each thread owns a disjoint set of blocks and records the last version it
// wrote. After the storm, both the cache and the device must hold exactly the
// latest version of every block.
TEST_P(ConcurrencyTest, DisjointWritersSurviveEvictionStorm) {
  constexpr int kThreads = 6;
  constexpr BlockId kBlocksPerThread = 20;
  constexpr int kOps = 3000;
  const BlockId total_blocks = kThreads * kBlocksPerThread;
  MemoryBlockDevice dev(kBlockSize, total_blocks);
  std::vector<std::vector<std::uint32_t>> versions(
      kThreads, std::vector<std::uint32_t>(kBlocksPerThread, 0));
  {
    BlockCache cache(&dev, MakeOptions());
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&, t] {
        std::mt19937 rng(300 + t);
        for (int i = 0; i < kOps; ++i) {
          const BlockId local = rng() % kBlocksPerThread;
          const BlockId id = static_cast<BlockId>(t) * kBlocksPerThread + local;
          if (rng() % 4 == 0) {
            std::vector<std::uint8_t> out(kBlockSize);
            Status s = cache.Read(id, 0, out.data(), out.size());
            if (s.ok()) {
              ASSERT_EQ(out, MakeBlock(kBlockSize, id, versions[t][local]))
                  << "thread " << t << " block " << id;
            } else {
              ASSERT_EQ(s.code(), Status::Code::kNoSpace);
            }
            continue;
          }
          const std::uint32_t next = versions[t][local] + 1;
          auto data = MakeBlock(kBlockSize, id, next);
          Status s = cache.Write(id, 0, data.data(), data.size());
          if (s.ok()) {
            versions[t][local] = next;
          } else {
            ASSERT_EQ(s.code(), Status::Code::kNoSpace);
          }
        }
      });
    }
    for (auto& th : threads) th.join();

    for (int t = 0; t < kThreads; ++t) {
      for (BlockId local = 0; local < kBlocksPerThread; ++local) {
        const BlockId id = static_cast<BlockId>(t) * kBlocksPerThread + local;
        std::vector<std::uint8_t> out(kBlockSize);
        ASSERT_TRUE(cache.Read(id, 0, out.data(), out.size()).ok());
        EXPECT_EQ(out, MakeBlock(kBlockSize, id, versions[t][local])) << "block " << id;
      }
    }
  }  // destructor flushes

  for (int t = 0; t < kThreads; ++t) {
    for (BlockId local = 0; local < kBlocksPerThread; ++local) {
      const BlockId id = static_cast<BlockId>(t) * kBlocksPerThread + local;
      EXPECT_EQ(PeekBlock(dev, id), MakeBlock(kBlockSize, id, versions[t][local]))
          << "device block " << id;
    }
  }
}

// A long-lived pin must protect its block from eviction while other threads
// churn through many more blocks than the cache holds.
TEST_P(ConcurrencyTest, PinnedBlockSurvivesChurn) {
  MemoryBlockDevice dev(kBlockSize, 512);
  BlockCache cache(&dev, MakeOptions());
  auto data = MakeBlock(kBlockSize, 0, 42);
  ASSERT_TRUE(cache.Write(0, 0, data.data(), data.size()).ok());

  BlockHandle pinned;
  ASSERT_TRUE(cache.Pin(0, &pinned).ok());

  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      std::uint8_t b;
      for (BlockId id = 1 + t; id < 512; id += 4) {
        Status s = cache.Read(id, 0, &b, 1);
        if (!s.ok()) ASSERT_EQ(s.code(), Status::Code::kNoSpace);
      }
    });
  }
  for (auto& th : threads) th.join();

  EXPECT_TRUE(cache.Contains(0));
  auto lock = pinned.ReadLock();
  EXPECT_EQ(std::memcmp(pinned.data(), data.data(), kBlockSize), 0);
}

// With a slow device and many threads racing on the same blocks, every device
// read must correspond to exactly one miss (no duplicate loads).
TEST_P(ConcurrencyTest, EachMissIssuesExactlyOneDeviceRead) {
  FaultyDevice dev(kBlockSize, 64);
  dev.SetLatency(std::chrono::microseconds(200));
  BlockCache cache(&dev, MakeOptions());

  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(400 + t);
      std::uint8_t b;
      for (int i = 0; i < 300; ++i) {
        Status s = cache.Read(rng() % 16, 0, &b, 1);
        if (!s.ok()) ASSERT_EQ(s.code(), Status::Code::kNoSpace);
      }
    });
  }
  for (auto& th : threads) th.join();

  CacheStats s = cache.stats();
  EXPECT_EQ(dev.reads(), s.misses);
  EXPECT_EQ(s.load_failures, 0u);
}

INSTANTIATE_TEST_SUITE_P(
    Configs, ConcurrencyTest,
    ::testing::Values(Config{"Roomy", 256, 8, false},
                      Config{"Pressured", 12, 4, false},
                      Config{"PressuredWithFlusher", 12, 4, true}),
    [](const ::testing::TestParamInfo<Config>& info) { return info.param.name; });

}  // namespace
}  // namespace hotblock
