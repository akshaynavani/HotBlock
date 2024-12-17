#include "hotblock/block_pool.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace hotblock {

namespace {

std::size_t RoundUp(std::size_t n, std::size_t align) { return (n + align - 1) / align * align; }

bool IsPowerOfTwo(std::size_t n) { return n != 0 && (n & (n - 1)) == 0; }

}  // namespace

BlockPool::BlockPool(std::size_t frame_size, std::size_t num_frames, std::size_t alignment)
    : frame_size_(RoundUp(frame_size, alignment)), num_frames_(num_frames) {
  if (frame_size == 0 || num_frames == 0) {
    throw std::invalid_argument("BlockPool: frame_size and num_frames must be non-zero");
  }
  if (!IsPowerOfTwo(alignment) || alignment < sizeof(void*)) {
    throw std::invalid_argument("BlockPool: alignment must be a power of two >= sizeof(void*)");
  }
  if (num_frames > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("BlockPool: too many frames");
  }

  void* mem = nullptr;
  if (::posix_memalign(&mem, alignment, frame_size_ * num_frames_) != 0) {
    throw std::bad_alloc();
  }
  slab_ = static_cast<std::uint8_t*>(mem);
  std::memset(slab_, 0, frame_size_ * num_frames_);

  // Push in reverse so the first allocation hands out frame 0 (nicer locality).
  free_list_.reserve(num_frames_);
  for (std::size_t i = num_frames_; i-- > 0;) {
    free_list_.push_back(static_cast<std::uint32_t>(i));
  }
#ifndef NDEBUG
  allocated_.assign(num_frames_, false);
#endif
}

BlockPool::~BlockPool() { std::free(slab_); }

std::uint8_t* BlockPool::Allocate() {
  std::lock_guard<std::mutex> lock(mu_);
  if (free_list_.empty()) return nullptr;
  const std::uint32_t idx = free_list_.back();
  free_list_.pop_back();
#ifndef NDEBUG
  assert(!allocated_[idx]);
  allocated_[idx] = true;
#endif
  return slab_ + static_cast<std::size_t>(idx) * frame_size_;
}

void BlockPool::Free(std::uint8_t* frame) {
  if (frame == nullptr) return;
  assert(Owns(frame));
  const std::size_t idx = IndexOf(frame);
  std::lock_guard<std::mutex> lock(mu_);
#ifndef NDEBUG
  assert(allocated_[idx] && "double free of pool frame");
  allocated_[idx] = false;
#endif
  free_list_.push_back(static_cast<std::uint32_t>(idx));
}

bool BlockPool::Owns(const std::uint8_t* p) const {
  if (p < slab_ || p >= slab_ + frame_size_ * num_frames_) return false;
  return static_cast<std::size_t>(p - slab_) % frame_size_ == 0;
}

std::size_t BlockPool::available() const {
  std::lock_guard<std::mutex> lock(mu_);
  return free_list_.size();
}

std::size_t BlockPool::IndexOf(const std::uint8_t* frame) const {
  return static_cast<std::size_t>(frame - slab_) / frame_size_;
}

}  // namespace hotblock
