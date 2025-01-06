#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace hotblock {

// Point-in-time snapshot of cache counters.
struct CacheStats {
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t evictions = 0;
  std::uint64_t writebacks = 0;      // dirty blocks written to the device
  std::uint64_t load_waits = 0;      // lookups that waited on another thread's load
  std::uint64_t load_failures = 0;
  std::uint64_t flusher_passes = 0;  // background FlushAll runs
  std::uint64_t dirty_blocks = 0;    // currently dirty (gauge, not a counter)
  std::uint64_t resident_blocks = 0; // currently cached (gauge)

  double hit_rate() const {
    const std::uint64_t total = hits + misses;
    return total == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(total);
  }

  std::string ToString() const {
    return "hits=" + std::to_string(hits) + " misses=" + std::to_string(misses) +
           " evictions=" + std::to_string(evictions) + " writebacks=" + std::to_string(writebacks) +
           " load_waits=" + std::to_string(load_waits) +
           " load_failures=" + std::to_string(load_failures) +
           " flusher_passes=" + std::to_string(flusher_passes) +
           " dirty=" + std::to_string(dirty_blocks) + " resident=" + std::to_string(resident_blocks);
  }
};

namespace detail {

// Relaxed atomic counters shared by all shards. Exact ordering between
// counters is not required; they are monotonic telemetry.
struct AtomicCacheStats {
  std::atomic<std::uint64_t> hits{0};
  std::atomic<std::uint64_t> misses{0};
  std::atomic<std::uint64_t> evictions{0};
  std::atomic<std::uint64_t> writebacks{0};
  std::atomic<std::uint64_t> load_waits{0};
  std::atomic<std::uint64_t> load_failures{0};
  std::atomic<std::uint64_t> flusher_passes{0};
  std::atomic<std::int64_t> dirty_blocks{0};
  std::atomic<std::int64_t> resident_blocks{0};

  static void Inc(std::atomic<std::uint64_t>& c) { c.fetch_add(1, std::memory_order_relaxed); }

  CacheStats Snapshot() const {
    CacheStats s;
    s.hits = hits.load(std::memory_order_relaxed);
    s.misses = misses.load(std::memory_order_relaxed);
    s.evictions = evictions.load(std::memory_order_relaxed);
    s.writebacks = writebacks.load(std::memory_order_relaxed);
    s.load_waits = load_waits.load(std::memory_order_relaxed);
    s.load_failures = load_failures.load(std::memory_order_relaxed);
    s.flusher_passes = flusher_passes.load(std::memory_order_relaxed);
    const auto d = dirty_blocks.load(std::memory_order_relaxed);
    const auto r = resident_blocks.load(std::memory_order_relaxed);
    s.dirty_blocks = d < 0 ? 0 : static_cast<std::uint64_t>(d);
    s.resident_blocks = r < 0 ? 0 : static_cast<std::uint64_t>(r);
    return s;
  }
};

}  // namespace detail
}  // namespace hotblock
