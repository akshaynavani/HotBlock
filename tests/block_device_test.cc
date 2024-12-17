#include "hotblock/block_device.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

namespace hotblock {
namespace {

constexpr std::size_t kBlockSize = 512;

std::vector<std::uint8_t> Pattern(std::uint8_t seed) {
  std::vector<std::uint8_t> v(kBlockSize);
  for (std::size_t i = 0; i < v.size(); ++i) v[i] = static_cast<std::uint8_t>(seed + i);
  return v;
}

TEST(MemoryBlockDeviceTest, StartsZeroed) {
  MemoryBlockDevice dev(kBlockSize, 4);
  std::vector<std::uint8_t> buf(kBlockSize, 0xff);
  ASSERT_TRUE(dev.ReadBlock(3, buf.data()).ok());
  for (auto b : buf) EXPECT_EQ(b, 0);
}

TEST(MemoryBlockDeviceTest, RoundTripsAndCountsIo) {
  MemoryBlockDevice dev(kBlockSize, 8);
  auto data = Pattern(7);
  ASSERT_TRUE(dev.WriteBlock(5, data.data()).ok());
  std::vector<std::uint8_t> out(kBlockSize);
  ASSERT_TRUE(dev.ReadBlock(5, out.data()).ok());
  EXPECT_EQ(out, data);
  EXPECT_EQ(dev.reads(), 1u);
  EXPECT_EQ(dev.writes(), 1u);
}

TEST(MemoryBlockDeviceTest, RejectsOutOfRange) {
  MemoryBlockDevice dev(kBlockSize, 2);
  std::vector<std::uint8_t> buf(kBlockSize);
  Status s = dev.ReadBlock(2, buf.data());
  EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
  s = dev.WriteBlock(100, buf.data());
  EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
}

class FileBlockDeviceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/hotblock_dev_XXXXXX";
    int fd = ::mkstemp(tmpl);
    ASSERT_GE(fd, 0);
    ::close(fd);
    path_ = tmpl;
  }
  void TearDown() override { std::remove(path_.c_str()); }

  std::string path_;
};

TEST_F(FileBlockDeviceTest, RoundTripsAcrossReopen) {
  auto data = Pattern(42);
  {
    std::unique_ptr<FileBlockDevice> dev;
    ASSERT_TRUE(FileBlockDevice::Open(path_, kBlockSize, 16, &dev).ok());
    ASSERT_TRUE(dev->WriteBlock(9, data.data()).ok());
    ASSERT_TRUE(dev->Sync().ok());
  }
  std::unique_ptr<FileBlockDevice> dev;
  ASSERT_TRUE(FileBlockDevice::Open(path_, kBlockSize, 16, &dev).ok());
  std::vector<std::uint8_t> out(kBlockSize);
  ASSERT_TRUE(dev->ReadBlock(9, out.data()).ok());
  EXPECT_EQ(out, data);
}

TEST_F(FileBlockDeviceTest, RejectsZeroGeometry) {
  std::unique_ptr<FileBlockDevice> dev;
  EXPECT_EQ(FileBlockDevice::Open(path_, 0, 16, &dev).code(), Status::Code::kInvalidArgument);
}

}  // namespace
}  // namespace hotblock
