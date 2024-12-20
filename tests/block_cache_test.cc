#include "hotblock/block_cache.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "test_util.h"

namespace hotblock {
namespace {

using testing_util::FaultyDevice;
using testing_util::MakeBlock;

constexpr std::size_t kBlockSize = 256;

CacheOptions SmallCache(std::size_t capacity, std::size_t shards = 1) {
  CacheOptions o;
  o.capacity_blocks = capacity;
  o.num_shards = shards;
  return o;
}

TEST(BlockCacheTest, MissThenHit) {
  MemoryBlockDevice dev(kBlockSize, 64);
  auto expected = MakeBlock(kBlockSize, 3, 1);
  ASSERT_TRUE(dev.WriteBlock(3, expected.data()).ok());

  BlockCache cache(&dev, SmallCache(8));
  std::vector<std::uint8_t> out(kBlockSize);
  ASSERT_TRUE(cache.Read(3, 0, out.data(), out.size()).ok());
  EXPECT_EQ(out, expected);
  ASSERT_TRUE(cache.Read(3, 0, out.data(), out.size()).ok());

  CacheStats s = cache.stats();
  EXPECT_EQ(s.misses, 1u);
  EXPECT_EQ(s.hits, 1u);
  EXPECT_EQ(s.resident_blocks, 1u);
  EXPECT_EQ(dev.reads(), 1u);  // the hit never touched the device
}

TEST(BlockCacheTest, PartialReadAndWrite) {
  MemoryBlockDevice dev(kBlockSize, 4);
  BlockCache cache(&dev, SmallCache(4));
  const char msg[] = "hotblock";
  ASSERT_TRUE(cache.Write(1, 100, msg, sizeof(msg)).ok());
  char out[sizeof(msg)] = {};
  ASSERT_TRUE(cache.Read(1, 100, out, sizeof(out)).ok());
  EXPECT_STREQ(out, msg);
}

TEST(BlockCacheTest, RejectsOutOfRangeRequests) {
  MemoryBlockDevice dev(kBlockSize, 4);
  BlockCache cache(&dev, SmallCache(4));
  std::uint8_t buf[8];
  EXPECT_EQ(cache.Read(4, 0, buf, 1).code(), Status::Code::kInvalidArgument);
  EXPECT_EQ(cache.Read(0, kBlockSize - 2, buf, 4).code(), Status::Code::kInvalidArgument);
  BlockHandle h;
  EXPECT_EQ(cache.Pin(99, &h).code(), Status::Code::kInvalidArgument);
  EXPECT_FALSE(h.valid());
}

TEST(BlockCacheTest, HandleIsMovableAndReleasesOnce) {
  MemoryBlockDevice dev(kBlockSize, 4);
  BlockCache cache(&dev, SmallCache(1));
  BlockHandle a;
  ASSERT_TRUE(cache.Pin(0, &a).ok());
  BlockHandle b = std::move(a);
  EXPECT_FALSE(a.valid());
  ASSERT_TRUE(b.valid());
  EXPECT_EQ(b.id(), 0u);
  b.Release();
  b.Release();  // idempotent
  // The single frame is free again, so a different block can be loaded.
  BlockHandle c;
  EXPECT_TRUE(cache.Pin(1, &c).ok());
}

TEST(BlockCacheTest, ShardCapacitiesSumToTotal) {
  MemoryBlockDevice dev(kBlockSize, 1024);
  BlockCache cache(&dev, SmallCache(10, 4));
  EXPECT_EQ(cache.num_shards(), 4u);
  EXPECT_EQ(cache.capacity(), 10u);
  // Touch far more blocks than fit; resident count must never exceed capacity.
  std::uint8_t b;
  for (BlockId id = 0; id < 200; ++id) ASSERT_TRUE(cache.Read(id, 0, &b, 1).ok());
  EXPECT_LE(cache.stats().resident_blocks, 10u);
}

TEST(BlockCacheTest, ShardsClampedToCapacity) {
  MemoryBlockDevice dev(kBlockSize, 16);
  BlockCache cache(&dev, SmallCache(3, 16));
  EXPECT_EQ(cache.num_shards(), 3u);
}

TEST(BlockCacheTest, LoadFailureIsReportedAndNotCached) {
  FaultyDevice dev(kBlockSize, 8);
  dev.FailReads(5);
  BlockCache cache(&dev, SmallCache(4));
  BlockHandle h;
  EXPECT_EQ(cache.Pin(5, &h).code(), Status::Code::kIoError);
  EXPECT_FALSE(cache.Contains(5));
  EXPECT_EQ(cache.stats().load_failures, 1u);
  EXPECT_EQ(cache.stats().resident_blocks, 0u);

  dev.HealReads(5);
  EXPECT_TRUE(cache.Pin(5, &h).ok());
}

TEST(BlockCacheTest, ConcurrentMissesOnSameBlockLoadOnce) {
  FaultyDevice dev(kBlockSize, 8);
  dev.SetLatency(std::chrono::milliseconds(20));
  BlockCache cache(&dev, SmallCache(4));

  constexpr int kThreads = 8;
  std::atomic<int> ok{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      std::uint8_t b;
      if (cache.Read(2, 0, &b, 1).ok()) ok.fetch_add(1);
    });
  }
  for (auto& th : threads) th.join();

  EXPECT_EQ(ok.load(), kThreads);
  EXPECT_EQ(dev.reads(), 1u);
  CacheStats s = cache.stats();
  EXPECT_EQ(s.misses, 1u);
  EXPECT_EQ(s.hits, static_cast<std::uint64_t>(kThreads - 1));
}

TEST(BlockCacheTest, DataWrittenThroughHandleIsVisibleToOtherHandles) {
  MemoryBlockDevice dev(kBlockSize, 4);
  BlockCache cache(&dev, SmallCache(4));
  BlockHandle w, r;
  ASSERT_TRUE(cache.Pin(0, &w).ok());
  ASSERT_TRUE(cache.Pin(0, &r).ok());
  {
    auto lock = w.WriteLock();
    std::memset(w.mutable_data(), 0xAB, w.size());
    w.MarkDirty();
  }
  auto lock = r.ReadLock();
  EXPECT_EQ(r.data()[0], 0xAB);
  EXPECT_EQ(r.data()[kBlockSize - 1], 0xAB);
  EXPECT_TRUE(r.dirty());
}

}  // namespace
}  // namespace hotblock
