#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace hotblock {

// Fixed-size frame allocator backed by a single aligned slab.
//
// All block buffers the cache hands out come from here, so steady-state cache
// operation performs zero heap allocations for block data and frames stay
// aligned for O_DIRECT-style I/O. Thread-safe.
class BlockPool {
 public:
  static constexpr std::size_t kDefaultAlignment = 4096;

  BlockPool(std::size_t frame_size, std::size_t num_frames,
            std::size_t alignment = kDefaultAlignment);
  ~BlockPool();

  BlockPool(const BlockPool&) = delete;
  BlockPool& operator=(const BlockPool&) = delete;

  // Returns a frame of frame_size() bytes, or nullptr when exhausted.
  std::uint8_t* Allocate();
  // Returns a frame to the pool. `frame` must have come from this pool.
  void Free(std::uint8_t* frame);

  bool Owns(const std::uint8_t* p) const;

  std::size_t frame_size() const { return frame_size_; }
  std::size_t capacity() const { return num_frames_; }
  std::size_t available() const;
  std::size_t in_use() const { return capacity() - available(); }

 private:
  std::size_t IndexOf(const std::uint8_t* frame) const;

  const std::size_t frame_size_;   // rounded up to alignment
  const std::size_t num_frames_;
  std::uint8_t* slab_ = nullptr;

  mutable std::mutex mu_;
  std::vector<std::uint32_t> free_list_;  // stack of free frame indices
#ifndef NDEBUG
  std::vector<bool> allocated_;           // double-free detection in debug builds
#endif
};

}  // namespace hotblock
