#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "hotblock/block_cache.h"
#include "test_util.h"

namespace hotblock {
namespace {

using testing_util::FaultyDevice;
using testing_util::MakeBlock;
using testing_util::PeekBlock;

constexpr std::size_t kBlockSize = 256;

CacheOptions Options(std::size_t capacity, WritePolicy policy = WritePolicy::kWriteBack) {
  CacheOptions o;
  o.capacity_blocks = capacity;
  o.num_shards = 4;
  o.write_policy = policy;
  return o;
}

// Polls `pred` for up to `timeout`. Avoids flaky fixed sleeps in flusher tests.
template <typename Pred>
bool Eventually(Pred pred, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return pred();
}

TEST(WriteBackTest, WritesStayInCacheUntilFlushed) {
  MemoryBlockDevice dev(kBlockSize, 32);
  BlockCache cache(&dev, Options(16));
  for (BlockId id = 0; id < 8; ++id) {
    auto d = MakeBlock(kBlockSize, id, 1);
    ASSERT_TRUE(cache.Write(id, 0, d.data(), d.size()).ok());
  }
  EXPECT_EQ(dev.writes(), 0u);
  EXPECT_EQ(cache.stats().dirty_blocks, 8u);

  ASSERT_TRUE(cache.FlushAll().ok());
  EXPECT_EQ(dev.writes(), 8u);
  EXPECT_EQ(cache.stats().dirty_blocks, 0u);
  for (BlockId id = 0; id < 8; ++id) EXPECT_EQ(PeekBlock(dev, id), MakeBlock(kBlockSize, id, 1));
}

TEST(WriteBackTest, RepeatedWritesCoalesceIntoOneDeviceWrite) {
  MemoryBlockDevice dev(kBlockSize, 4);
  BlockCache cache(&dev, Options(4));
  for (std::uint32_t v = 0; v < 100; ++v) {
    ASSERT_TRUE(cache.Write(1, 0, &v, sizeof(v)).ok());
  }
  ASSERT_TRUE(cache.FlushAll().ok());
  EXPECT_EQ(dev.writes(), 1u);
  std::uint32_t got = 0;
  auto raw = PeekBlock(dev, 1);
  std::memcpy(&got, raw.data(), sizeof(got));
  EXPECT_EQ(got, 99u);
}

TEST(WriteBackTest, FlushAllOnCleanCacheIsNoop) {
  MemoryBlockDevice dev(kBlockSize, 8);
  BlockCache cache(&dev, Options(8));
  std::uint8_t b;
  ASSERT_TRUE(cache.Read(2, 0, &b, 1).ok());
  ASSERT_TRUE(cache.FlushAll().ok());
  EXPECT_EQ(dev.writes(), 0u);
}

TEST(WriteBackTest, FlushSingleBlock) {
  MemoryBlockDevice dev(kBlockSize, 8);
  BlockCache cache(&dev, Options(8));
  auto a = MakeBlock(kBlockSize, 0, 1);
  auto b = MakeBlock(kBlockSize, 1, 1);
  ASSERT_TRUE(cache.Write(0, 0, a.data(), a.size()).ok());
  ASSERT_TRUE(cache.Write(1, 0, b.data(), b.size()).ok());

  ASSERT_TRUE(cache.Flush(0).ok());
  EXPECT_EQ(dev.writes(), 1u);
  EXPECT_EQ(PeekBlock(dev, 0), a);
  EXPECT_EQ(cache.stats().dirty_blocks, 1u);

  // Non-resident and already-clean blocks are no-ops.
  EXPECT_TRUE(cache.Flush(5).ok());
  EXPECT_TRUE(cache.Flush(0).ok());
  EXPECT_EQ(dev.writes(), 1u);
}

TEST(WriteBackTest, FlushedBlockStaysResidentAndClean) {
  MemoryBlockDevice dev(kBlockSize, 8);
  BlockCache cache(&dev, Options(8));
  std::uint8_t v = 9;
  ASSERT_TRUE(cache.Write(3, 0, &v, 1).ok());
  ASSERT_TRUE(cache.FlushAll().ok());
  EXPECT_TRUE(cache.Contains(3));
  BlockHandle h;
  ASSERT_TRUE(cache.Pin(3, &h).ok());
  EXPECT_FALSE(h.dirty());
}

TEST(WriteBackTest, FailedFlushKeepsBlockDirtyAndRetries) {
  FaultyDevice dev(kBlockSize, 8);
  BlockCache cache(&dev, Options(8));
  auto d = MakeBlock(kBlockSize, 4, 2);
  ASSERT_TRUE(cache.Write(4, 0, d.data(), d.size()).ok());

  dev.FailWrites(4);
  EXPECT_EQ(cache.FlushAll().code(), Status::Code::kIoError);
  EXPECT_EQ(cache.stats().dirty_blocks, 1u);

  dev.HealWrites(4);
  ASSERT_TRUE(cache.FlushAll().ok());
  EXPECT_EQ(cache.stats().dirty_blocks, 0u);
  EXPECT_EQ(PeekBlock(dev.inner(), 4), d);
}

TEST(WriteBackTest, DestructorFlushesDirtyBlocks) {
  MemoryBlockDevice dev(kBlockSize, 8);
  auto d = MakeBlock(kBlockSize, 6, 5);
  {
    BlockCache cache(&dev, Options(8));
    ASSERT_TRUE(cache.Write(6, 0, d.data(), d.size()).ok());
    EXPECT_EQ(dev.writes(), 0u);
  }
  EXPECT_EQ(PeekBlock(dev, 6), d);
}

TEST(WriteBackTest, SyncFlushesToFileDevice) {
  char tmpl[] = "/tmp/hotblock_wb_XXXXXX";
  int fd = ::mkstemp(tmpl);
  ASSERT_GE(fd, 0);
  ::close(fd);
  auto d = MakeBlock(kBlockSize, 2, 11);
  {
    std::unique_ptr<FileBlockDevice> dev;
    ASSERT_TRUE(FileBlockDevice::Open(tmpl, kBlockSize, 8, &dev).ok());
    BlockCache cache(dev.get(), Options(4));
    ASSERT_TRUE(cache.Write(2, 0, d.data(), d.size()).ok());
    ASSERT_TRUE(cache.Sync().ok());
  }
  std::unique_ptr<FileBlockDevice> dev;
  ASSERT_TRUE(FileBlockDevice::Open(tmpl, kBlockSize, 8, &dev).ok());
  std::vector<std::uint8_t> out(kBlockSize);
  ASSERT_TRUE(dev->ReadBlock(2, out.data()).ok());
  EXPECT_EQ(out, d);
  std::remove(tmpl);
}

TEST(WriteThroughTest, EveryWriteReachesTheDevice) {
  MemoryBlockDevice dev(kBlockSize, 8);
  BlockCache cache(&dev, Options(8, WritePolicy::kWriteThrough));
  auto d = MakeBlock(kBlockSize, 1, 1);
  ASSERT_TRUE(cache.Write(1, 0, d.data(), d.size()).ok());
  EXPECT_EQ(dev.writes(), 1u);
  EXPECT_EQ(PeekBlock(dev, 1), d);
  EXPECT_EQ(cache.stats().dirty_blocks, 0u);

  std::uint8_t v = 0x5a;
  ASSERT_TRUE(cache.Write(1, 10, &v, 1).ok());
  EXPECT_EQ(dev.writes(), 2u);
}

TEST(WriteThroughTest, HandleWritesFlushOnRelease) {
  MemoryBlockDevice dev(kBlockSize, 8);
  BlockCache cache(&dev, Options(8, WritePolicy::kWriteThrough));
  {
    BlockHandle h;
    ASSERT_TRUE(cache.Pin(0, &h).ok());
    {
      auto lock = h.WriteLock();
      std::memset(h.mutable_data(), 0x11, h.size());
      h.MarkDirty();
    }
    EXPECT_EQ(dev.writes(), 0u);
  }
  EXPECT_EQ(dev.writes(), 1u);
  EXPECT_EQ(PeekBlock(dev, 0)[0], 0x11);
}

TEST(WriteThroughTest, ReportsDeviceErrors) {
  FaultyDevice dev(kBlockSize, 8);
  dev.FailWrites(3);
  BlockCache cache(&dev, Options(8, WritePolicy::kWriteThrough));
  std::uint8_t v = 1;
  EXPECT_EQ(cache.Write(3, 0, &v, 1).code(), Status::Code::kIoError);
  dev.HealWrites(3);
}

TEST(BackgroundFlusherTest, FlushesOnInterval) {
  MemoryBlockDevice dev(kBlockSize, 16);
  CacheOptions o = Options(16);
  o.background_flush = true;
  o.flush_interval = std::chrono::milliseconds(10);
  o.flush_dirty_ratio = 1.0;  // only the timer can trigger
  BlockCache cache(&dev, o);

  auto d = MakeBlock(kBlockSize, 7, 1);
  ASSERT_TRUE(cache.Write(7, 0, d.data(), d.size()).ok());
  EXPECT_TRUE(Eventually([&] { return cache.stats().dirty_blocks == 0; }));
  EXPECT_EQ(PeekBlock(dev, 7), d);
  EXPECT_GE(cache.stats().flusher_passes, 1u);
}

TEST(BackgroundFlusherTest, DirtyRatioWakesFlusherEarly) {
  MemoryBlockDevice dev(kBlockSize, 64);
  CacheOptions o = Options(32);
  o.background_flush = true;
  o.flush_interval = std::chrono::hours(1);  // effectively never on a timer
  o.flush_dirty_ratio = 0.25;                // 8 of 32 frames
  BlockCache cache(&dev, o);

  for (BlockId id = 0; id < 7; ++id) {
    std::uint8_t v = static_cast<std::uint8_t>(id);
    ASSERT_TRUE(cache.Write(id, 0, &v, 1).ok());
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_EQ(dev.writes(), 0u);  // below threshold: flusher stays asleep

  std::uint8_t v = 7;
  ASSERT_TRUE(cache.Write(7, 0, &v, 1).ok());
  EXPECT_TRUE(Eventually([&] { return dev.writes() >= 8; }));
}

TEST(BackgroundFlusherTest, NotStartedForWriteThrough) {
  MemoryBlockDevice dev(kBlockSize, 8);
  CacheOptions o = Options(8, WritePolicy::kWriteThrough);
  o.background_flush = true;
  o.flush_interval = std::chrono::milliseconds(1);
  BlockCache cache(&dev, o);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(cache.stats().flusher_passes, 0u);
}

}  // namespace
}  // namespace hotblock
