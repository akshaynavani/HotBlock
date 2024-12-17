#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "hotblock/types.h"

namespace hotblock {

// Backing store addressed in fixed-size blocks. Implementations must be safe to
// call concurrently from multiple threads for *different* blocks; the cache
// guarantees it never issues two overlapping I/Os for the same block.
class BlockDevice {
 public:
  virtual ~BlockDevice() = default;

  virtual std::size_t block_size() const = 0;
  virtual std::uint64_t num_blocks() const = 0;

  // Reads exactly block_size() bytes of block `id` into `buf`.
  virtual Status ReadBlock(BlockId id, std::uint8_t* buf) = 0;
  // Writes exactly block_size() bytes from `buf` to block `id`.
  virtual Status WriteBlock(BlockId id, const std::uint8_t* buf) = 0;
  // Makes previously written blocks durable.
  virtual Status Sync() { return Status::Ok(); }

  // I/O counters, useful for asserting write-back behaviour in tests.
  std::uint64_t reads() const { return reads_.load(std::memory_order_relaxed); }
  std::uint64_t writes() const { return writes_.load(std::memory_order_relaxed); }

 protected:
  Status CheckRange(BlockId id) const;

  std::atomic<std::uint64_t> reads_{0};
  std::atomic<std::uint64_t> writes_{0};
};

// Volatile, RAM-backed device. Each block has its own mutex so that a reader
// never observes a half-written block even if a caller misbehaves.
class MemoryBlockDevice final : public BlockDevice {
 public:
  MemoryBlockDevice(std::size_t block_size, std::uint64_t num_blocks);

  std::size_t block_size() const override { return block_size_; }
  std::uint64_t num_blocks() const override { return num_blocks_; }

  Status ReadBlock(BlockId id, std::uint8_t* buf) override;
  Status WriteBlock(BlockId id, const std::uint8_t* buf) override;

  // Direct access for test assertions, bypassing the I/O counters.
  void Peek(BlockId id, std::uint8_t* buf) const;

 private:
  static constexpr std::size_t kLockStripes = 64;

  std::mutex& StripeFor(BlockId id) const { return stripes_[id % kLockStripes]; }

  const std::size_t block_size_;
  const std::uint64_t num_blocks_;
  std::vector<std::uint8_t> data_;
  mutable std::mutex stripes_[kLockStripes];
};

// File-backed device using POSIX pread/pwrite, which are thread-safe with
// respect to the file offset.
class FileBlockDevice final : public BlockDevice {
 public:
  // Opens (creating if needed) `path` and sizes it to block_size * num_blocks.
  static Status Open(const std::string& path, std::size_t block_size, std::uint64_t num_blocks,
                     std::unique_ptr<FileBlockDevice>* out);
  ~FileBlockDevice() override;

  FileBlockDevice(const FileBlockDevice&) = delete;
  FileBlockDevice& operator=(const FileBlockDevice&) = delete;

  std::size_t block_size() const override { return block_size_; }
  std::uint64_t num_blocks() const override { return num_blocks_; }

  Status ReadBlock(BlockId id, std::uint8_t* buf) override;
  Status WriteBlock(BlockId id, const std::uint8_t* buf) override;
  Status Sync() override;

 private:
  FileBlockDevice(int fd, std::size_t block_size, std::uint64_t num_blocks);

  int fd_;
  const std::size_t block_size_;
  const std::uint64_t num_blocks_;
};

}  // namespace hotblock
