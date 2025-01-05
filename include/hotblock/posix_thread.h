#pragma once

#include <pthread.h>

#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

namespace hotblock {

// Minimal RAII wrapper over a POSIX thread. Used for the cache's background
// flusher so the thread can carry a recognisable name in debuggers / top -H.
class PosixThread {
 public:
  PosixThread(std::function<void()> fn, std::string name)
      : fn_(std::move(fn)), name_(std::move(name)) {
    const int rc = ::pthread_create(&tid_, nullptr, &PosixThread::Trampoline, this);
    if (rc != 0) {
      throw std::runtime_error(std::string("pthread_create failed: ") + std::strerror(rc));
    }
    joinable_ = true;
  }

  ~PosixThread() { Join(); }

  PosixThread(const PosixThread&) = delete;
  PosixThread& operator=(const PosixThread&) = delete;

  void Join() {
    if (joinable_) {
      ::pthread_join(tid_, nullptr);
      joinable_ = false;
    }
  }

 private:
  static void* Trampoline(void* arg) {
    auto* self = static_cast<PosixThread*>(arg);
    // Thread names are limited to 15 chars + NUL on Linux.
    const std::string short_name = self->name_.substr(0, 15);
#if defined(__APPLE__)
    ::pthread_setname_np(short_name.c_str());
#elif defined(__linux__)
    ::pthread_setname_np(::pthread_self(), short_name.c_str());
#endif
    self->fn_();
    return nullptr;
  }

  std::function<void()> fn_;
  std::string name_;
  pthread_t tid_{};
  bool joinable_ = false;
};

}  // namespace hotblock
