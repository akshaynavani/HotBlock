#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "hotblock/block_device.h"
#include "hotblock/block_pool.h"
#include "hotblock/cache_options.h"
#include "hotblock/cache_stats.h"
#include "hotblock/types.h"

namespace hotblock {

class BlockCache;

namespace detail {
struct Entry;
struct Shard;
}  // namespace detail

// RAII pin on a cached block. While a handle is alive the block cannot be
// evicted. Access to the bytes is coordinated with the block's reader/writer
// lock:
//
//   BlockHandle h;
//   cache.Pin(id, &h);
//   {
//     auto lock = h.WriteLock();
//     std::memcpy(h.mutable_data(), src, n);
//     h.MarkDirty();
//   }
//
// Handles are move-only and must not outlive the cache.
class BlockHandle {
 public:
  BlockHandle() = default;
  ~BlockHandle() { Release(); }

  BlockHandle(BlockHandle&& other) noexcept;
  BlockHandle& operator=(BlockHandle&& other) noexcept;
  BlockHandle(const BlockHandle&) = delete;
  BlockHandle& operator=(const BlockHandle&) = delete;

  bool valid() const { return entry_ != nullptr; }
  BlockId id() const;
  std::size_t size() const;

  // Shared lock for reading the block bytes.
  std::shared_lock<std::shared_mutex> ReadLock() const;
  // Exclusive lock for modifying the block bytes.
  std::unique_lock<std::shared_mutex> WriteLock();

  // Pointer to the block bytes. Only dereference while holding a lock.
  const std::uint8_t* data() const;
  std::uint8_t* mutable_data();

  // Records that the block was modified. Call while holding WriteLock().
  void MarkDirty();
  bool dirty() const;

  // Drops the pin early. Safe to call more than once.
  void Release();

 private:
  friend class BlockCache;
  BlockHandle(BlockCache* cache, detail::Entry* entry) : cache_(cache), entry_(entry) {}

  BlockCache* cache_ = nullptr;
  detail::Entry* entry_ = nullptr;
};

// Thread-safe, sharded LRU cache of fixed-size blocks in front of a BlockDevice.
//
// Locking protocol:
//   * Shard mutex   — guards the shard's map, LRU list, pin counts and entry
//                     state. Never held while waiting on a block data lock.
//   * Entry data_mu — std::shared_mutex guarding the block bytes.
//
// Only unpinned, fully loaded entries sit on a shard's LRU list, so eviction is
// a pop from the list tail. A miss publishes a kLoading placeholder and performs
// the device read without the shard lock; concurrent lookups of the same block
// wait for that single load instead of issuing their own.
class BlockCache {
 public:
  // `device` must outlive the cache.
  BlockCache(BlockDevice* device, const CacheOptions& options);
  ~BlockCache();

  BlockCache(const BlockCache&) = delete;
  BlockCache& operator=(const BlockCache&) = delete;

  // Pins block `id`, loading it from the device on a miss.
  // Returns kNoSpace if every frame in the target shard is pinned.
  Status Pin(BlockId id, BlockHandle* out);

  // Convenience byte-range helpers built on Pin + data locks.
  Status Read(BlockId id, std::size_t offset, void* dst, std::size_t len);
  Status Write(BlockId id, std::size_t offset, const void* src, std::size_t len);

  // True if `id` is resident (loaded or loading). Does not affect recency.
  bool Contains(BlockId id) const;

  std::size_t block_size() const { return device_->block_size(); }
  std::size_t capacity() const { return capacity_; }
  std::size_t num_shards() const { return shards_.size(); }
  const CacheOptions& options() const { return options_; }
  CacheStats stats() const { return stats_.Snapshot(); }

 private:
  friend class BlockHandle;

  detail::Shard& ShardFor(BlockId id) const;
  Status CheckRange(BlockId id, std::size_t offset, std::size_t len) const;

  // Finds a free entry for `shard`, evicting the LRU unpinned block if needed.
  // Requires shard.mu held. Returns nullptr and sets *status on failure.
  detail::Entry* AcquireEntryLocked(detail::Shard& shard, Status* status);
  // Returns an entry's frame to the pool and the entry to the shard free list.
  void ReleaseEntryLocked(detail::Shard& shard, detail::Entry* e);

  void Unpin(detail::Entry* e);

  BlockDevice* const device_;
  const CacheOptions options_;
  std::size_t capacity_ = 0;
  BlockPool pool_;
  std::vector<std::unique_ptr<detail::Shard>> shards_;
  mutable detail::AtomicCacheStats stats_;
};

}  // namespace hotblock
