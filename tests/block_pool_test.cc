#include "hotblock/block_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <set>
#include <thread>
#include <vector>

namespace hotblock {
namespace {

TEST(BlockPoolTest, FramesAreAlignedAndDistinct) {
  BlockPool pool(4096, 8);
  std::set<std::uint8_t*> seen;
  for (int i = 0; i < 8; ++i) {
    std::uint8_t* f = pool.Allocate();
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(f) % BlockPool::kDefaultAlignment, 0u);
    EXPECT_TRUE(pool.Owns(f));
    EXPECT_TRUE(seen.insert(f).second);
  }
  EXPECT_EQ(pool.available(), 0u);
  EXPECT_EQ(pool.Allocate(), nullptr);
  for (auto* f : seen) pool.Free(f);
  EXPECT_EQ(pool.available(), 8u);
}

TEST(BlockPoolTest, RoundsFrameSizeUpToAlignment) {
  BlockPool pool(100, 4, 64);
  EXPECT_EQ(pool.frame_size(), 128u);
  std::uint8_t* a = pool.Allocate();
  std::uint8_t* b = pool.Allocate();
  EXPECT_EQ(static_cast<std::size_t>(b - a), 128u);
  pool.Free(a);
  pool.Free(b);
}

TEST(BlockPoolTest, FreedFramesAreReused) {
  BlockPool pool(512, 1);
  std::uint8_t* a = pool.Allocate();
  ASSERT_NE(a, nullptr);
  pool.Free(a);
  EXPECT_EQ(pool.Allocate(), a);
  pool.Free(a);
}

TEST(BlockPoolTest, OwnsRejectsForeignPointers) {
  BlockPool pool(512, 2);
  std::uint8_t local[16];
  EXPECT_FALSE(pool.Owns(local));
  std::uint8_t* f = pool.Allocate();
  EXPECT_FALSE(pool.Owns(f + 1));  // interior pointer
  pool.Free(f);
}

TEST(BlockPoolTest, RejectsBadGeometry) {
  EXPECT_THROW(BlockPool(0, 4), std::invalid_argument);
  EXPECT_THROW(BlockPool(512, 0), std::invalid_argument);
  EXPECT_THROW(BlockPool(512, 4, 3), std::invalid_argument);
}

TEST(BlockPoolTest, ConcurrentAllocateFreeNeverHandsOutSameFrameTwice) {
  constexpr int kThreads = 8;
  constexpr int kIters = 20000;
  BlockPool pool(256, 16);
  // One owner flag per frame; a second owner would observe the flag set.
  std::vector<std::atomic<int>> owners(pool.capacity());
  for (auto& o : owners) o.store(0);
  std::atomic<bool> conflict{false};

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < kIters; ++i) {
        std::uint8_t* f = pool.Allocate();
        if (f == nullptr) continue;
        // Frames are contiguous, so (address / frame_size) mod capacity is a
        // unique slot per frame.
        auto& flag = owners[static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(f) /
                                                     pool.frame_size()) %
                            pool.capacity()];
        if (flag.exchange(1) != 0) conflict.store(true);
        flag.store(0);
        pool.Free(f);
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_FALSE(conflict.load());
  EXPECT_EQ(pool.available(), pool.capacity());
}

}  // namespace
}  // namespace hotblock
