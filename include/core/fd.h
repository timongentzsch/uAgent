// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_FD_H_
#define UAGENT_INCLUDE_CORE_FD_H_
// An owned file descriptor: ownership is a type rather than a close(2) on
// every early return, so an error path cannot forget one.

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <string>
#include <utility>

namespace uagent {

class Fd {
 public:
  Fd() = default;
  explicit Fd(int fd) : fd_(fd) {}
  ~Fd() { Reset(); }

  Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) Reset(std::exchange(other.fd_, -1));
    return *this;
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;

  int Get() const { return fd_; }
  bool Valid() const { return fd_ >= 0; }
  explicit operator bool() const { return Valid(); }

  // Close now, optionally adopting a replacement.
  void Reset(int fd = -1) {
    if (fd_ >= 0 && fd_ != fd) close(fd_);
    fd_ = fd;
  }

  // Hand the descriptor to a caller that takes over closing it.
  [[nodiscard]] int Release() { return std::exchange(fd_, -1); }

  // For durable writes, which are only committed once close(2) has succeeded:
  // a deferred write can fail there and nowhere else.
  [[nodiscard]] int Close() {
    const int fd = std::exchange(fd_, -1);
    return fd >= 0 ? close(fd) : 0;
  }

  // An independent descriptor for a caller with its own lifetime, such as a
  // writer racing the owning thread's close.
  [[nodiscard]] Fd Duplicate() const {
    return Fd(fd_ >= 0 ? fcntl(fd_, F_DUPFD_CLOEXEC, 0) : -1);
  }

 private:
  int fd_ = -1;
};

// A blocking, close-on-exec stream connection to a local socket path; empty
// when the path does not fit or nobody is listening.
inline Fd ConnectUnix(const std::string& path) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.empty() || path.size() >= sizeof(address.sun_path)) return {};
  path.copy(address.sun_path, path.size());
  Fd fd(socket(AF_UNIX, SOCK_STREAM, 0));
  if (!fd) return {};
  fcntl(fd.Get(), F_SETFD, FD_CLOEXEC);
  if (connect(fd.Get(), reinterpret_cast<sockaddr*>(&address),
              sizeof(address)) != 0) {
    return {};
  }
  return fd;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_FD_H_
