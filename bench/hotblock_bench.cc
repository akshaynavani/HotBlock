// Multi-threaded throughput benchmark for BlockCache.
//
// Usage:
//   hotblock_bench [--threads N] [--capacity BLOCKS] [--shards N]
//                  [--working-set BLOCKS] [--block-size BYTES]
//                  [--read-ratio 0..1] [--seconds S] [--zipf THETA]
//                  [--policy back|through] [--flusher] [--file PATH]
//
// Accesses follow a Zipf-like distribution over the working set so that a
// realistic fraction of requests hit hot blocks.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "hotblock/block_cache.h"
#include "hotblock/block_device.h"

using namespace hotblock;

namespace {

struct Args {
  int threads = 4;
  std::size_t capacity = 4096;
  std::size_t shards = 16;
  std::uint64_t working_set = 16384;
  std::size_t block_size = 4096;
  double read_ratio = 0.8;
  double seconds = 3.0;
  double zipf = 0.99;
  WritePolicy policy = WritePolicy::kWriteBack;
  bool flusher = false;
  std::string file;
};

[[noreturn]] void Usage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s [--threads N] [--capacity BLOCKS] [--shards N] [--working-set BLOCKS]\n"
               "          [--block-size BYTES] [--read-ratio R] [--seconds S] [--zipf THETA]\n"
               "          [--policy back|through] [--flusher] [--file PATH]\n",
               prog);
  std::exit(2);
}

Args Parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) Usage(argv[0]);
      return argv[++i];
    };
    if (k == "--threads") a.threads = std::atoi(next());
    else if (k == "--capacity") a.capacity = std::strtoull(next(), nullptr, 10);
    else if (k == "--shards") a.shards = std::strtoull(next(), nullptr, 10);
    else if (k == "--working-set") a.working_set = std::strtoull(next(), nullptr, 10);
    else if (k == "--block-size") a.block_size = std::strtoull(next(), nullptr, 10);
    else if (k == "--read-ratio") a.read_ratio = std::atof(next());
    else if (k == "--seconds") a.seconds = std::atof(next());
    else if (k == "--zipf") a.zipf = std::atof(next());
    else if (k == "--policy") {
      std::string p = next();
      if (p == "back") a.policy = WritePolicy::kWriteBack;
      else if (p == "through") a.policy = WritePolicy::kWriteThrough;
      else Usage(argv[0]);
    } else if (k == "--flusher") a.flusher = true;
    else if (k == "--file") a.file = next();
    else Usage(argv[0]);
  }
  if (a.threads <= 0 || a.capacity == 0 || a.working_set == 0 || a.block_size == 0) Usage(argv[0]);
  return a;
}

// Zipf sampler using a precomputed CDF and binary search.
class Zipf {
 public:
  Zipf(std::uint64_t n, double theta) : cdf_(n) {
    double sum = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
      sum += 1.0 / std::pow(static_cast<double>(i + 1), theta);
      cdf_[i] = sum;
    }
    for (auto& c : cdf_) c /= sum;
  }

  template <typename Rng>
  std::uint64_t operator()(Rng& rng) const {
    const double u = std::uniform_real_distribution<double>(0.0, 1.0)(rng);
    auto it = std::lower_bound(cdf_.begin(), cdf_.end(), u);
    return static_cast<std::uint64_t>(it - cdf_.begin());
  }

 private:
  std::vector<double> cdf_;
};

}  // namespace

int main(int argc, char** argv) {
  const Args args = Parse(argc, argv);

  std::unique_ptr<BlockDevice> device;
  if (args.file.empty()) {
    device = std::make_unique<MemoryBlockDevice>(args.block_size, args.working_set);
  } else {
    std::unique_ptr<FileBlockDevice> f;
    Status s = FileBlockDevice::Open(args.file, args.block_size, args.working_set, &f);
    if (!s.ok()) {
      std::fprintf(stderr, "open %s: %s\n", args.file.c_str(), s.ToString().c_str());
      return 1;
    }
    device = std::move(f);
  }

  CacheOptions opts;
  opts.capacity_blocks = args.capacity;
  opts.num_shards = args.shards;
  opts.write_policy = args.policy;
  opts.background_flush = args.flusher;

  BlockCache cache(device.get(), opts);
  const Zipf zipf(args.working_set, args.zipf);
  // Scramble ranks so hot blocks are spread across shards, not clustered at 0.
  std::vector<BlockId> perm(args.working_set);
  for (std::uint64_t i = 0; i < args.working_set; ++i) perm[i] = i;
  std::shuffle(perm.begin(), perm.end(), std::mt19937_64(12345));

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> ops{0};
  std::atomic<std::uint64_t> errors{0};
  std::vector<std::thread> workers;
  for (int t = 0; t < args.threads; ++t) {
    workers.emplace_back([&, t] {
      std::mt19937_64 rng(static_cast<std::uint64_t>(t) * 7919 + 1);
      std::uniform_real_distribution<double> coin(0.0, 1.0);
      std::vector<std::uint8_t> buf(args.block_size, static_cast<std::uint8_t>(t));
      std::uint64_t local = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        const BlockId id = perm[zipf(rng)];
        Status s = coin(rng) < args.read_ratio
                       ? cache.Read(id, 0, buf.data(), buf.size())
                       : cache.Write(id, 0, buf.data(), buf.size());
        if (!s.ok()) errors.fetch_add(1, std::memory_order_relaxed);
        ++local;
      }
      ops.fetch_add(local, std::memory_order_relaxed);
    });
  }

  const auto start = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::duration<double>(args.seconds));
  stop.store(true);
  for (auto& w : workers) w.join();
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  const CacheStats st = cache.stats();
  std::printf("threads=%d capacity=%zu shards=%zu working_set=%llu block_size=%zu read_ratio=%.2f "
              "zipf=%.2f policy=%s flusher=%s\n",
              args.threads, args.capacity, cache.num_shards(),
              static_cast<unsigned long long>(args.working_set), args.block_size, args.read_ratio,
              args.zipf, args.policy == WritePolicy::kWriteBack ? "write-back" : "write-through",
              args.flusher ? "on" : "off");
  std::printf("ops=%llu elapsed=%.2fs throughput=%.0f ops/s errors=%llu\n",
              static_cast<unsigned long long>(ops.load()), elapsed,
              static_cast<double>(ops.load()) / elapsed,
              static_cast<unsigned long long>(errors.load()));
  std::printf("hit_rate=%.4f %s\n", st.hit_rate(), st.ToString().c_str());
  std::printf("device_reads=%llu device_writes=%llu\n",
              static_cast<unsigned long long>(device->reads()),
              static_cast<unsigned long long>(device->writes()));
  return 0;
}
