#include "hotblock/block_cache.h"

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

#include "hotblock/lru_list.h"

namespace hotblock {
namespace detail {

enum class EntryState : std::uint8_t {
  kFree,     // on the shard free list
  kLoading,  // published in the map, device read in flight
  kReady,    // contents valid
};

struct Entry : LruNode {
  BlockId id = 0;
  std::uint8_t* frame = nullptr;
  Shard* shard = nullptr;

  // Guarded by shard->mu.
  std::uint32_t pins = 0;
  EntryState state = EntryState::kFree;

  // Set by writers under data_mu; cleared by flushers under data_mu (shared).
  std::atomic<bool> dirty{false};
  std::shared_mutex data_mu;
};

struct Shard {
  std::mutex mu;
  // Signalled when a load completes (successfully or not).
  std::condition_variable load_cv;

  std::unordered_map<BlockId, Entry*> map;
  LruList<Entry> lru;  // unpinned kReady entries, MRU at front
  std::unique_ptr<Entry[]> entries;
  std::vector<Entry*> free_entries;
  std::size_t capacity = 0;
};

}  // namespace detail

using detail::Entry;
using detail::EntryState;
using detail::Shard;

namespace {

std::size_t NormalizeCapacity(const CacheOptions& o) {
  if (o.capacity_blocks == 0) throw std::invalid_argument("capacity_blocks must be > 0");
  return o.capacity_blocks;
}

std::size_t NormalizeShards(const CacheOptions& o) {
  std::size_t n = o.num_shards == 0 ? 1 : o.num_shards;
  return std::min(n, o.capacity_blocks);
}

// SplitMix64 finalizer: spreads sequential block ids across shards.
std::uint64_t MixHash(std::uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

}  // namespace

// ---------------------------------------------------------------------------
// BlockHandle
// ---------------------------------------------------------------------------

BlockHandle::BlockHandle(BlockHandle&& other) noexcept
    : cache_(other.cache_), entry_(other.entry_) {
  other.cache_ = nullptr;
  other.entry_ = nullptr;
}

BlockHandle& BlockHandle::operator=(BlockHandle&& other) noexcept {
  if (this != &other) {
    Release();
    cache_ = other.cache_;
    entry_ = other.entry_;
    other.cache_ = nullptr;
    other.entry_ = nullptr;
  }
  return *this;
}

BlockId BlockHandle::id() const {
  assert(valid());
  return entry_->id;
}

std::size_t BlockHandle::size() const {
  assert(valid());
  return cache_->block_size();
}

std::shared_lock<std::shared_mutex> BlockHandle::ReadLock() const {
  assert(valid());
  return std::shared_lock<std::shared_mutex>(entry_->data_mu);
}

std::unique_lock<std::shared_mutex> BlockHandle::WriteLock() {
  assert(valid());
  return std::unique_lock<std::shared_mutex>(entry_->data_mu);
}

const std::uint8_t* BlockHandle::data() const {
  assert(valid());
  return entry_->frame;
}

std::uint8_t* BlockHandle::mutable_data() {
  assert(valid());
  return entry_->frame;
}

void BlockHandle::MarkDirty() {
  assert(valid());
  if (!entry_->dirty.exchange(true, std::memory_order_acq_rel)) {
    cache_->stats_.dirty_blocks.fetch_add(1, std::memory_order_relaxed);
    cache_->OnBlockDirtied();
  }
}

bool BlockHandle::dirty() const {
  assert(valid());
  return entry_->dirty.load(std::memory_order_acquire);
}

void BlockHandle::Release() {
  if (entry_ != nullptr) {
    cache_->Unpin(entry_);
    entry_ = nullptr;
    cache_ = nullptr;
  }
}

// ---------------------------------------------------------------------------
// BlockCache
// ---------------------------------------------------------------------------

BlockCache::BlockCache(BlockDevice* device, const CacheOptions& options)
    : device_(device),
      options_(options),
      capacity_(NormalizeCapacity(options)),
      pool_(device->block_size(), capacity_) {
  const std::size_t n = NormalizeShards(options);
  shards_.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    auto shard = std::make_unique<Shard>();
    // Spread the remainder over the first shards so capacities sum exactly.
    shard->capacity = capacity_ / n + (i < capacity_ % n ? 1 : 0);
    shard->entries.reset(new Entry[shard->capacity]);
    shard->free_entries.reserve(shard->capacity);
    for (std::size_t k = shard->capacity; k-- > 0;) {
      shard->entries[k].shard = shard.get();
      shard->free_entries.push_back(&shard->entries[k]);
    }
    shard->map.reserve(shard->capacity * 2);
    shards_.push_back(std::move(shard));
  }
}

BlockCache::~BlockCache() {
  // Do not lose dirty data on shutdown. Errors cannot be reported from a
  // destructor; callers that care should call Sync() first.
  (void)FlushAll();
  for (auto& shard : shards_) {
    std::lock_guard<std::mutex> lock(shard->mu);
    for (auto& kv : shard->map) {
      assert(kv.second->pins == 0 && "BlockCache destroyed with outstanding handles");
      pool_.Free(kv.second->frame);
      kv.second->frame = nullptr;
    }
  }
}

Shard& BlockCache::ShardFor(BlockId id) const {
  return *shards_[MixHash(id) % shards_.size()];
}

Status BlockCache::CheckRange(BlockId id, std::size_t offset, std::size_t len) const {
  if (id >= device_->num_blocks()) {
    return Status::InvalidArgument("block " + std::to_string(id) + " out of range");
  }
  if (offset > block_size() || len > block_size() - offset) {
    return Status::InvalidArgument("byte range exceeds block size");
  }
  return Status::Ok();
}

Entry* BlockCache::AcquireEntryLocked(Shard& shard, Status* status) {
  if (!shard.free_entries.empty()) {
    Entry* e = shard.free_entries.back();
    shard.free_entries.pop_back();
    e->frame = pool_.Allocate();
    // Shard capacities sum to the pool size, so this cannot fail.
    assert(e->frame != nullptr);
    stats_.resident_blocks.fetch_add(1, std::memory_order_relaxed);
    return e;
  }

  Entry* victim = shard.lru.Back();
  if (victim == nullptr) {
    *status = Status::NoSpace("all " + std::to_string(shard.capacity) +
                              " frames in shard are pinned");
    return nullptr;
  }
  assert(victim->pins == 0 && victim->state == EntryState::kReady);

  // Unpinned entries have no live handles, so nobody can hold data_mu; the
  // write-back below can read the frame without taking it.
  if (victim->dirty.load(std::memory_order_acquire)) {
    Status s = device_->WriteBlock(victim->id, victim->frame);
    if (!s.ok()) {
      // Keep the dirty block cached rather than lose data; rotate it so the
      // next eviction attempt tries a different victim.
      shard.lru.MoveToFront(victim);
      *status = s;
      return nullptr;
    }
    victim->dirty.store(false, std::memory_order_release);
    stats_.dirty_blocks.fetch_sub(1, std::memory_order_relaxed);
    detail::AtomicCacheStats::Inc(stats_.writebacks);
  }

  shard.lru.Remove(victim);
  shard.map.erase(victim->id);
  victim->state = EntryState::kFree;
  detail::AtomicCacheStats::Inc(stats_.evictions);
  return victim;  // frame is reused as-is
}

void BlockCache::ReleaseEntryLocked(Shard& shard, Entry* e) {
  pool_.Free(e->frame);
  e->frame = nullptr;
  e->state = EntryState::kFree;
  e->pins = 0;
  e->dirty.store(false, std::memory_order_relaxed);
  shard.free_entries.push_back(e);
  stats_.resident_blocks.fetch_sub(1, std::memory_order_relaxed);
}

Status BlockCache::Pin(BlockId id, BlockHandle* out) {
  if (id >= device_->num_blocks()) {
    return Status::InvalidArgument("block " + std::to_string(id) + " out of range");
  }
  out->Release();
  Shard& shard = ShardFor(id);
  std::unique_lock<std::mutex> lock(shard.mu);

  for (;;) {
    auto it = shard.map.find(id);
    if (it != shard.map.end()) {
      Entry* e = it->second;
      if (e->state == EntryState::kLoading) {
        // Another thread is reading this block. Wait, then re-lookup: the
        // load may have failed and the entry may have been recycled.
        detail::AtomicCacheStats::Inc(stats_.load_waits);
        shard.load_cv.wait(lock);
        continue;
      }
      if (e->pins == 0) shard.lru.Remove(e);
      ++e->pins;
      detail::AtomicCacheStats::Inc(stats_.hits);
      *out = BlockHandle(this, e);
      return Status::Ok();
    }

    // Miss: claim a frame and publish a loading placeholder.
    Status status;
    Entry* e = AcquireEntryLocked(shard, &status);
    if (e == nullptr) return status;
    e->id = id;
    e->state = EntryState::kLoading;
    e->pins = 1;
    e->dirty.store(false, std::memory_order_relaxed);
    shard.map.emplace(id, e);
    detail::AtomicCacheStats::Inc(stats_.misses);

    lock.unlock();
    Status read = device_->ReadBlock(id, e->frame);
    lock.lock();

    if (!read.ok()) {
      shard.map.erase(id);
      ReleaseEntryLocked(shard, e);
      detail::AtomicCacheStats::Inc(stats_.load_failures);
      shard.load_cv.notify_all();
      return read;
    }
    e->state = EntryState::kReady;
    shard.load_cv.notify_all();
    *out = BlockHandle(this, e);
    return Status::Ok();
  }
}

void BlockCache::OnBlockDirtied() {}

Status BlockCache::FlushEntry(Entry* e) {
  std::shared_lock<std::shared_mutex> data_lock(e->data_mu);
  // Clear before writing: a writer cannot run concurrently (we hold the lock
  // shared), and any write after we release re-dirties the block.
  if (!e->dirty.exchange(false, std::memory_order_acq_rel)) return Status::Ok();
  Status s = device_->WriteBlock(e->id, e->frame);
  if (!s.ok()) {
    e->dirty.store(true, std::memory_order_release);
    return s;
  }
  stats_.dirty_blocks.fetch_sub(1, std::memory_order_relaxed);
  detail::AtomicCacheStats::Inc(stats_.writebacks);
  return Status::Ok();
}

Status BlockCache::Flush(BlockId id) {
  if (id >= device_->num_blocks()) {
    return Status::InvalidArgument("block " + std::to_string(id) + " out of range");
  }
  Shard& shard = ShardFor(id);
  Entry* e = nullptr;
  {
    std::unique_lock<std::mutex> lock(shard.mu);
    for (;;) {
      auto it = shard.map.find(id);
      if (it == shard.map.end()) return Status::Ok();  // not resident: nothing to do
      e = it->second;
      if (e->state != EntryState::kLoading) break;
      shard.load_cv.wait(lock);  // a loading block is clean by definition, but wait for it
    }
    if (!e->dirty.load(std::memory_order_acquire)) return Status::Ok();
    if (e->pins == 0) shard.lru.Remove(e);
    ++e->pins;
  }
  Status s = FlushEntry(e);
  Unpin(e);
  return s;
}

Status BlockCache::FlushAll() {
  Status first_error;
  std::vector<Entry*> batch;
  for (auto& shard_ptr : shards_) {
    Shard& shard = *shard_ptr;
    batch.clear();
    {
      // Pin every dirty entry so it cannot be evicted while we write it.
      std::lock_guard<std::mutex> lock(shard.mu);
      for (auto& kv : shard.map) {
        Entry* e = kv.second;
        if (e->state != EntryState::kReady || !e->dirty.load(std::memory_order_acquire)) continue;
        if (e->pins == 0) shard.lru.Remove(e);
        ++e->pins;
        batch.push_back(e);
      }
    }
    // Write in block order: friendlier to the device than hash order.
    std::sort(batch.begin(), batch.end(), [](const Entry* a, const Entry* b) { return a->id < b->id; });
    for (Entry* e : batch) {
      Status s = FlushEntry(e);
      if (!s.ok() && first_error.ok()) first_error = s;
      Unpin(e);
    }
  }
  return first_error;
}

Status BlockCache::Sync() {
  Status s = FlushAll();
  if (!s.ok()) return s;
  return device_->Sync();
}

void BlockCache::Unpin(Entry* e) {
  // Write-through: persist before dropping the pin so the entry cannot be
  // evicted mid-write. Best effort; on failure the block stays dirty and is
  // retried on eviction or the next flush.
  if (options_.write_policy == WritePolicy::kWriteThrough &&
      e->dirty.load(std::memory_order_acquire)) {
    (void)FlushEntry(e);
  }
  Shard& shard = *e->shard;
  std::lock_guard<std::mutex> lock(shard.mu);
  assert(e->pins > 0);
  if (--e->pins == 0) shard.lru.PushFront(e);
}

Status BlockCache::Read(BlockId id, std::size_t offset, void* dst, std::size_t len) {
  Status s = CheckRange(id, offset, len);
  if (!s.ok()) return s;
  BlockHandle h;
  s = Pin(id, &h);
  if (!s.ok()) return s;
  auto lock = h.ReadLock();
  std::memcpy(dst, h.data() + offset, len);
  return Status::Ok();
}

Status BlockCache::Write(BlockId id, std::size_t offset, const void* src, std::size_t len) {
  Status s = CheckRange(id, offset, len);
  if (!s.ok()) return s;
  BlockHandle h;
  s = Pin(id, &h);
  if (!s.ok()) return s;
  {
    auto lock = h.WriteLock();
    std::memcpy(h.mutable_data() + offset, src, len);
    h.MarkDirty();
  }
  if (options_.write_policy == WritePolicy::kWriteThrough) {
    // Flush here (rather than in Unpin) so the caller sees I/O errors.
    return FlushEntry(h.entry_);
  }
  return Status::Ok();
}

bool BlockCache::Contains(BlockId id) const {
  Shard& shard = ShardFor(id);
  std::lock_guard<std::mutex> lock(shard.mu);
  return shard.map.count(id) != 0;
}

}  // namespace hotblock
