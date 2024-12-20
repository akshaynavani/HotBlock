#pragma once

#include <chrono>
#include <cstddef>

namespace hotblock {

enum class WritePolicy {
  // Modified blocks stay dirty in the cache and are written to the device on
  // eviction, explicit Flush/FlushAll, or by the background flusher.
  kWriteBack,
  // Modified blocks are written to the device when the last writer releases
  // its handle; the cache never holds dirty data for long.
  kWriteThrough,
};

struct CacheOptions {
  // Total number of block frames the cache may hold.
  std::size_t capacity_blocks = 1024;
  // Number of independently locked shards. Clamped to capacity_blocks.
  std::size_t num_shards = 16;

  WritePolicy write_policy = WritePolicy::kWriteBack;
};

}  // namespace hotblock
