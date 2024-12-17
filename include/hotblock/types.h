#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace hotblock {

// Logical block number on the backing device.
using BlockId = std::uint64_t;

// Lightweight status object. Errors are values, not exceptions, on the hot path.
class Status {
 public:
  enum class Code : std::uint8_t {
    kOk = 0,
    kNotFound,
    kInvalidArgument,
    kIoError,
    kNoSpace,  // every frame in the shard is pinned
    kShutdown,
  };

  Status() = default;

  static Status Ok() { return Status(); }
  static Status NotFound(std::string msg) { return Status(Code::kNotFound, std::move(msg)); }
  static Status InvalidArgument(std::string msg) {
    return Status(Code::kInvalidArgument, std::move(msg));
  }
  static Status IoError(std::string msg) { return Status(Code::kIoError, std::move(msg)); }
  static Status NoSpace(std::string msg) { return Status(Code::kNoSpace, std::move(msg)); }
  static Status Shutdown(std::string msg) { return Status(Code::kShutdown, std::move(msg)); }

  bool ok() const { return code_ == Code::kOk; }
  Code code() const { return code_; }
  const std::string& message() const { return message_; }

  std::string ToString() const {
    if (ok()) return "OK";
    return std::string(CodeName(code_)) + ": " + message_;
  }

  static const char* CodeName(Code c) {
    switch (c) {
      case Code::kOk: return "OK";
      case Code::kNotFound: return "NotFound";
      case Code::kInvalidArgument: return "InvalidArgument";
      case Code::kIoError: return "IoError";
      case Code::kNoSpace: return "NoSpace";
      case Code::kShutdown: return "Shutdown";
    }
    return "Unknown";
  }

 private:
  Status(Code code, std::string msg) : code_(code), message_(std::move(msg)) {}

  Code code_ = Code::kOk;
  std::string message_;
};

}  // namespace hotblock
