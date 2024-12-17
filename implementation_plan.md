# HotBlock — Implementation Plan

Thread-safe C++17 block cache with LRU eviction, write-back policy and memory-pooled
block allocations, validated by Google Test suites for coherence, concurrency and
eviction correctness.

## Key design decisions

- **Library + tests + bench**: a static library `hotblock`, a `hotblock_tests` Google Test
  binary (fetched via CMake `FetchContent`), and a small `hotblock_bench` driver.
- **Backing store abstraction**: `BlockDevice` interface with an in-memory implementation
  (tests) and a POSIX file implementation using `pread`/`pwrite`.
- **Memory pool**: one contiguous, aligned slab carved into fixed-size frames with a
  lock-protected free list. The cache never calls `new` per block on the hot path.
- **Sharding**: the cache is split into N shards (block id hashed). Each shard has its own
  mutex, hash map and intrusive LRU list, so unrelated blocks do not contend.
- **Two-level locking**: the shard mutex guards metadata (map, LRU, pin counts, state);
  a per-entry `std::shared_mutex` guards the block bytes (many readers / one writer).
- **Pinning**: `BlockHandle` is an RAII pin. Pinned blocks are never evicted.
- **Miss handling**: a miss inserts a `kLoading` placeholder, drops the shard lock while
  doing device I/O, and other threads wanting the same block wait on a condition variable
  (no duplicate loads, no torn reads).
- **Write policy**: write-back (default) marks frames dirty and writes them on eviction,
  explicit flush, or by a background flusher (a POSIX `pthread`); write-through writes
  on handle release.
- **Stats**: lock-free atomic counters (hits, misses, evictions, write-backs, dirty count).

## Phases

### Phase 1 — Build skeleton, core types, block devices
- `CMakeLists.txt` (C++17, `-pthread`, warnings, GoogleTest via FetchContent)
- `include/hotblock/types.h` — `BlockId`, `Status`
- `include/hotblock/block_device.h`, `src/block_device.cc` — `BlockDevice`,
  `MemoryBlockDevice`, `FileBlockDevice`
- `tests/block_device_test.cc`

### Phase 2 — Memory pool and intrusive LRU list
- `include/hotblock/block_pool.h`, `src/block_pool.cc` — aligned slab allocator
- `include/hotblock/lru_list.h` — intrusive doubly linked list
- `tests/block_pool_test.cc`, `tests/lru_list_test.cc`

### Phase 3 — Sharded cache core with LRU eviction
- `include/hotblock/cache_options.h`, `include/hotblock/cache_stats.h`
- `include/hotblock/block_cache.h`, `src/block_cache.cc` — shards, lookup, pin/unpin,
  miss loading with placeholder, LRU eviction of unpinned frames, `BlockHandle`
- `tests/block_cache_test.cc`, `tests/eviction_test.cc`

### Phase 4 — Write-back policy and background flusher
- Dirty tracking, `Flush(id)`, `FlushAll()`, write-back on eviction, write-through mode
- `include/hotblock/posix_thread.h` — thin `pthread` wrapper
- Background flusher thread (interval + dirty-ratio trigger)
- `tests/write_back_test.cc`

### Phase 5 — Concurrency suites and benchmark
- `tests/concurrency_test.cc` — coherence (concurrent read-modify-write on shared blocks),
  mixed reader/writer stress, eviction under load with integrity check, no double loads
- `bench/hotblock_bench.cc` — configurable threads / working set / read ratio, prints
  throughput and hit rate
- `.gitignore`, `README.md` with architecture diagram, run and test instructions
