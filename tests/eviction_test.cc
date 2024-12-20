#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "hotblock/block_cache.h"
#include "test_util.h"

namespace hotblock {
namespace {

using testing_util::FaultyDevice;
using testing_util::MakeBlock;
using testing_util::PeekBlock;

constexpr std::size_t kBlockSize = 128;

// Single shard so LRU order is global and fully deterministic.
CacheOptions OneShard(std::size_t capacity) {
  CacheOptions o;
  o.capacity_blocks = capacity;
  o.num_shards = 1;
  return o;
}

void Touch(BlockCache& cache, BlockId id) {
  std::uint8_t b;
  ASSERT_TRUE(cache.Read(id, 0, &b, 1).ok());
}

TEST(EvictionTest, EvictsLeastRecentlyUsed) {
  MemoryBlockDevice dev(kBlockSize, 16);
  BlockCache cache(&dev, OneShard(3));
  Touch(cache, 0);
  Touch(cache, 1);
  Touch(cache, 2);
  Touch(cache, 0);  // 1 is now the LRU block
  Touch(cache, 3);

  EXPECT_TRUE(cache.Contains(0));
  EXPECT_FALSE(cache.Contains(1));
  EXPECT_TRUE(cache.Contains(2));
  EXPECT_TRUE(cache.Contains(3));
  EXPECT_EQ(cache.stats().evictions, 1u);
}

TEST(EvictionTest, SequentialScanEvictsInInsertionOrder) {
  MemoryBlockDevice dev(kBlockSize, 64);
  BlockCache cache(&dev, OneShard(4));
  for (BlockId id = 0; id < 10; ++id) Touch(cache, id);
  for (BlockId id = 0; id < 6; ++id) EXPECT_FALSE(cache.Contains(id)) << id;
  for (BlockId id = 6; id < 10; ++id) EXPECT_TRUE(cache.Contains(id)) << id;
  EXPECT_EQ(cache.stats().evictions, 6u);
  EXPECT_EQ(cache.stats().resident_blocks, 4u);
}

TEST(EvictionTest, PinnedBlocksAreNeverEvicted) {
  MemoryBlockDevice dev(kBlockSize, 16);
  BlockCache cache(&dev, OneShard(2));
  BlockHandle a, b;
  ASSERT_TRUE(cache.Pin(0, &a).ok());
  ASSERT_TRUE(cache.Pin(1, &b).ok());

  BlockHandle c;
  EXPECT_EQ(cache.Pin(2, &c).code(), Status::Code::kNoSpace);
  EXPECT_TRUE(cache.Contains(0));
  EXPECT_TRUE(cache.Contains(1));

  a.Release();
  ASSERT_TRUE(cache.Pin(2, &c).ok());
  EXPECT_FALSE(cache.Contains(0));
  EXPECT_TRUE(cache.Contains(1));
}

TEST(EvictionTest, PinningRefreshesRecency) {
  MemoryBlockDevice dev(kBlockSize, 16);
  BlockCache cache(&dev, OneShard(2));
  Touch(cache, 0);
  Touch(cache, 1);
  {
    BlockHandle h;
    ASSERT_TRUE(cache.Pin(0, &h).ok());
  }  // 0 released after 1 -> 0 is MRU
  Touch(cache, 2);
  EXPECT_TRUE(cache.Contains(0));
  EXPECT_FALSE(cache.Contains(1));
}

TEST(EvictionTest, CleanVictimIsNotWrittenBack) {
  MemoryBlockDevice dev(kBlockSize, 16);
  BlockCache cache(&dev, OneShard(2));
  for (BlockId id = 0; id < 8; ++id) Touch(cache, id);
  EXPECT_EQ(dev.writes(), 0u);
  EXPECT_EQ(cache.stats().writebacks, 0u);
}

TEST(EvictionTest, DirtyVictimIsWrittenBackBeforeReuse) {
  MemoryBlockDevice dev(kBlockSize, 16);
  BlockCache cache(&dev, OneShard(2));
  auto data = MakeBlock(kBlockSize, 0, 7);
  ASSERT_TRUE(cache.Write(0, 0, data.data(), data.size()).ok());
  EXPECT_EQ(dev.writes(), 0u);  // write-back: nothing hits the device yet

  Touch(cache, 1);
  Touch(cache, 2);  // evicts 0
  EXPECT_FALSE(cache.Contains(0));
  EXPECT_EQ(dev.writes(), 1u);
  EXPECT_EQ(PeekBlock(dev, 0), data);
  CacheStats s = cache.stats();
  EXPECT_EQ(s.writebacks, 1u);
  EXPECT_EQ(s.dirty_blocks, 0u);
}

TEST(EvictionTest, EvictedBlockReloadsLatestContents) {
  MemoryBlockDevice dev(kBlockSize, 32);
  BlockCache cache(&dev, OneShard(3));
  for (BlockId id = 0; id < 20; ++id) {
    auto d = MakeBlock(kBlockSize, id, 1);
    ASSERT_TRUE(cache.Write(id, 0, d.data(), d.size()).ok());
  }
  for (BlockId id = 0; id < 20; ++id) {
    std::vector<std::uint8_t> out(kBlockSize);
    ASSERT_TRUE(cache.Read(id, 0, out.data(), out.size()).ok());
    EXPECT_EQ(out, MakeBlock(kBlockSize, id, 1)) << "block " << id;
  }
}

TEST(EvictionTest, FailedWriteBackKeepsDirtyBlockResident) {
  FaultyDevice dev(kBlockSize, 16);
  BlockCache cache(&dev, OneShard(1));
  auto data = MakeBlock(kBlockSize, 0, 3);
  ASSERT_TRUE(cache.Write(0, 0, data.data(), data.size()).ok());

  dev.FailWrites(0);
  BlockHandle h;
  EXPECT_EQ(cache.Pin(1, &h).code(), Status::Code::kIoError);
  EXPECT_TRUE(cache.Contains(0));
  EXPECT_EQ(cache.stats().dirty_blocks, 1u);

  dev.HealWrites(0);
  ASSERT_TRUE(cache.Pin(1, &h).ok());
  EXPECT_FALSE(cache.Contains(0));
  EXPECT_EQ(PeekBlock(dev.inner(), 0), data);
}

}  // namespace
}  // namespace hotblock
