#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "hotblock/block_device.h"

namespace hotblock {
namespace testing_util {

// MemoryBlockDevice wrapper that can inject failures and latency.
class FaultyDevice final : public BlockDevice {
 public:
  FaultyDevice(std::size_t block_size, std::uint64_t num_blocks)
      : inner_(block_size, num_blocks) {}

  std::size_t block_size() const override { return inner_.block_size(); }
  std::uint64_t num_blocks() const override { return inner_.num_blocks(); }

  Status ReadBlock(BlockId id, std::uint8_t* buf) override {
    Delay();
    if (Fails(fail_reads_, id)) return Status::IoError("injected read failure");
    reads_.fetch_add(1, std::memory_order_relaxed);
    return inner_.ReadBlock(id, buf);
  }

  Status WriteBlock(BlockId id, const std::uint8_t* buf) override {
    Delay();
    if (Fails(fail_writes_, id)) return Status::IoError("injected write failure");
    writes_.fetch_add(1, std::memory_order_relaxed);
    return inner_.WriteBlock(id, buf);
  }

  void FailReads(BlockId id) { Set(fail_reads_, id, true); }
  void FailWrites(BlockId id) { Set(fail_writes_, id, true); }
  void HealReads(BlockId id) { Set(fail_reads_, id, false); }
  void HealWrites(BlockId id) { Set(fail_writes_, id, false); }
  void SetLatency(std::chrono::microseconds d) { latency_us_.store(d.count()); }

  MemoryBlockDevice& inner() { return inner_; }

 private:
  void Delay() const {
    const auto us = latency_us_.load();
    if (us > 0) std::this_thread::sleep_for(std::chrono::microseconds(us));
  }
  bool Fails(const std::set<BlockId>& s, BlockId id) {
    std::lock_guard<std::mutex> lock(mu_);
    return s.count(id) != 0;
  }
  void Set(std::set<BlockId>& s, BlockId id, bool on) {
    std::lock_guard<std::mutex> lock(mu_);
    if (on) s.insert(id); else s.erase(id);
  }

  MemoryBlockDevice inner_;
  std::mutex mu_;
  std::set<BlockId> fail_reads_;
  std::set<BlockId> fail_writes_;
  std::atomic<std::int64_t> latency_us_{0};
};

// Deterministic block contents derived from (id, version).
inline std::vector<std::uint8_t> MakeBlock(std::size_t size, BlockId id, std::uint32_t version) {
  std::vector<std::uint8_t> v(size);
  std::uint64_t x = id * 0x9e3779b97f4a7c15ULL + version;
  for (std::size_t i = 0; i < size; ++i) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    v[i] = static_cast<std::uint8_t>(x);
  }
  return v;
}

inline std::vector<std::uint8_t> PeekBlock(const MemoryBlockDevice& dev, BlockId id) {
  std::vector<std::uint8_t> v(dev.block_size());
  dev.Peek(id, v.data());
  return v;
}

}  // namespace testing_util
}  // namespace hotblock
