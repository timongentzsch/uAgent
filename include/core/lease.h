// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_LEASE_H_
#define UAGENT_INCLUDE_CORE_LEASE_H_

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#include "include/core/fd.h"
#include "include/core/platform.h"

namespace uagent {

// Ownership follows an open descriptor, never a PID or a replaceable snapshot.
// Companion lock files must stay in place even after release/crash.
class FileLease {
 public:
  FileLease() = default;
  FileLease(const FileLease&) = delete;
  FileLease& operator=(const FileLease&) = delete;
  ~FileLease() { Reset(); }

  bool Owns(const std::string& path) const { return fd_ && path_ == path; }
  void Swap(FileLease& other) {
    std::swap(fd_, other.fd_);
    path_.swap(other.path_);
    std::swap(published_, other.published_);
  }

  bool Acquire(const std::string& path, std::string& error,
               bool publish_owner = false) {
    if (fd_ && path == path_) return true;
    Fd next(
        open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
    struct stat info{};
    if (!next || fstat(next.Get(), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != geteuid() || info.st_nlink != 1 ||
        (info.st_mode & 0077) != 0) {
      error = "cannot open private ownership file: " + path;
      return false;
    }
    int result;
    do {
      result = flock(next.Get(), LOCK_EX | LOCK_NB);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
      error = (errno == EWOULDBLOCK || errno == EAGAIN)
                  ? "already owned by another session: " + path
                  : "cannot acquire ownership: " + std::string(strerror(errno));
      return false;
    }
    Reset();
    fd_ = std::move(next);
    path_ = path;
    published_ = publish_owner;
    if (published_) {
      const pid_t pid = getpid();
      std::string identity = ProcessIdentity(pid);
      std::string owner =
          identity.empty() ? "" : std::to_string(pid) + " " + identity;
      // Presence is advisory. The descriptor lock remains the sole writer
      // authority even if the optional identity cannot be published.
      if (ftruncate(fd_.Get(), 0) == 0 && !owner.empty()) {
        ssize_t written = pwrite(fd_.Get(), owner.data(), owner.size(), 0);
        if (written != static_cast<ssize_t>(owner.size())) {
          ClearOwner();
        }
      }
    }
    return true;
  }

  // Never acquire a probe lock: even a brief shared lock can reject a real
  // writer racing to resume. The holder publishes its PID plus start identity
  // under the lease and clears it before releasing. A crash needs no cleanup.
  static bool HasLiveOwner(const std::string& path) {
    Fd file(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat info{};
    if (!file || fstat(file.Get(), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != geteuid() || info.st_nlink != 1 ||
        (info.st_mode & 0077)) {
      return false;
    }
    char buffer[128];
    ssize_t count = pread(file.Get(), buffer, sizeof(buffer), 0);
    if (count <= 0 || count == sizeof(buffer)) return false;
    std::string owner(buffer, static_cast<size_t>(count));
    size_t space = owner.find(' ');
    if (space == std::string::npos) return false;
    int64_t pid = 0;
    for (size_t i = 0; i < space; ++i) {
      if (owner[i] < '0' || owner[i] > '9' || pid > 214748364) return false;
      pid = pid * 10 + owner[i] - '0';
    }
    if (pid <= 0 || pid > INT32_MAX) return false;
    std::string identity = ProcessIdentity(static_cast<pid_t>(pid));
    return !identity.empty() && identity == owner.substr(space + 1);
  }

  void Reset() {
    if (published_ && fd_) ClearOwner();
    published_ = false;
    fd_.Reset();
    path_.clear();
  }

 private:
  void ClearOwner() {
    // Presence is advisory; closing the descriptor always releases ownership.
    [[maybe_unused]] const int result = ftruncate(fd_.Get(), 0);
  }
  Fd fd_;
  std::string path_;
  bool published_ = false;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_LEASE_H_
