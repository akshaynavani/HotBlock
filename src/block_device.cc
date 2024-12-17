#include "hotblock/block_device.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace hotblock {

namespace {

std::string ErrnoMessage(const char* what) {
  return std::string(what) + ": " + std::strerror(errno);
}

}  // namespace

Status BlockDevice::CheckRange(BlockId id) const {
  if (id >= num_blocks()) {
    return Status::InvalidArgument("block " + std::to_string(id) + " out of range (device has " +
                                   std::to_string(num_blocks()) + " blocks)");
  }
  return Status::Ok();
}

// ---------------------------------------------------------------------------
// MemoryBlockDevice
// ---------------------------------------------------------------------------

MemoryBlockDevice::MemoryBlockDevice(std::size_t block_size, std::uint64_t num_blocks)
    : block_size_(block_size),
      num_blocks_(num_blocks),
      data_(static_cast<std::size_t>(block_size * num_blocks), 0) {}

Status MemoryBlockDevice::ReadBlock(BlockId id, std::uint8_t* buf) {
  Status s = CheckRange(id);
  if (!s.ok()) return s;
  {
    std::lock_guard<std::mutex> lock(StripeFor(id));
    std::memcpy(buf, data_.data() + id * block_size_, block_size_);
  }
  reads_.fetch_add(1, std::memory_order_relaxed);
  return Status::Ok();
}

Status MemoryBlockDevice::WriteBlock(BlockId id, const std::uint8_t* buf) {
  Status s = CheckRange(id);
  if (!s.ok()) return s;
  {
    std::lock_guard<std::mutex> lock(StripeFor(id));
    std::memcpy(data_.data() + id * block_size_, buf, block_size_);
  }
  writes_.fetch_add(1, std::memory_order_relaxed);
  return Status::Ok();
}

void MemoryBlockDevice::Peek(BlockId id, std::uint8_t* buf) const {
  std::lock_guard<std::mutex> lock(StripeFor(id));
  std::memcpy(buf, data_.data() + id * block_size_, block_size_);
}

// ---------------------------------------------------------------------------
// FileBlockDevice
// ---------------------------------------------------------------------------

Status FileBlockDevice::Open(const std::string& path, std::size_t block_size,
                             std::uint64_t num_blocks, std::unique_ptr<FileBlockDevice>* out) {
  if (block_size == 0 || num_blocks == 0) {
    return Status::InvalidArgument("block_size and num_blocks must be non-zero");
  }
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) return Status::IoError(ErrnoMessage("open"));

  const off_t want = static_cast<off_t>(block_size * num_blocks);
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return Status::IoError(ErrnoMessage("fstat"));
  }
  if (st.st_size < want && ::ftruncate(fd, want) != 0) {
    ::close(fd);
    return Status::IoError(ErrnoMessage("ftruncate"));
  }
  out->reset(new FileBlockDevice(fd, block_size, num_blocks));
  return Status::Ok();
}

FileBlockDevice::FileBlockDevice(int fd, std::size_t block_size, std::uint64_t num_blocks)
    : fd_(fd), block_size_(block_size), num_blocks_(num_blocks) {}

FileBlockDevice::~FileBlockDevice() {
  if (fd_ >= 0) ::close(fd_);
}

Status FileBlockDevice::ReadBlock(BlockId id, std::uint8_t* buf) {
  Status s = CheckRange(id);
  if (!s.ok()) return s;
  std::size_t done = 0;
  const off_t base = static_cast<off_t>(id * block_size_);
  while (done < block_size_) {
    ssize_t n = ::pread(fd_, buf + done, block_size_ - done, base + static_cast<off_t>(done));
    if (n < 0) {
      if (errno == EINTR) continue;
      return Status::IoError(ErrnoMessage("pread"));
    }
    if (n == 0) {
      // Sparse tail: treat as zeros.
      std::memset(buf + done, 0, block_size_ - done);
      break;
    }
    done += static_cast<std::size_t>(n);
  }
  reads_.fetch_add(1, std::memory_order_relaxed);
  return Status::Ok();
}

Status FileBlockDevice::WriteBlock(BlockId id, const std::uint8_t* buf) {
  Status s = CheckRange(id);
  if (!s.ok()) return s;
  std::size_t done = 0;
  const off_t base = static_cast<off_t>(id * block_size_);
  while (done < block_size_) {
    ssize_t n = ::pwrite(fd_, buf + done, block_size_ - done, base + static_cast<off_t>(done));
    if (n < 0) {
      if (errno == EINTR) continue;
      return Status::IoError(ErrnoMessage("pwrite"));
    }
    done += static_cast<std::size_t>(n);
  }
  writes_.fetch_add(1, std::memory_order_relaxed);
  return Status::Ok();
}

Status FileBlockDevice::Sync() {
  if (::fsync(fd_) != 0) return Status::IoError(ErrnoMessage("fsync"));
  return Status::Ok();
}

}  // namespace hotblock
