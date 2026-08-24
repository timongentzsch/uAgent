// Copyright 2026 Timon Gentzsch

#include "include/core/file_watch.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <string>

#if defined(__APPLE__)
#include <sys/event.h>
#elif defined(__linux__)
#include <sys/inotify.h>
#endif

#include "include/core/fd.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/time.h"

namespace uagent {
namespace {

FileWaitResult CurrentInterrupt() {
  if (AbortRequested()) return FileWaitResult::kInterrupted;
  if (SteeringYieldRequested()) return FileWaitResult::kSteering;
  return FileWaitResult::kTimedOut;
}

FileWaitResult WaitFallback(std::chrono::steady_clock::time_point deadline) {
  int timeout_ms = std::min(100, PollTimeoutMs(deadline));
  pollfd events[2] = {{AbortWakeFd(), POLLIN, 0},
                      {SteeringWakeFd(), POLLIN, 0}};
  int ready;
  do {
    ready = poll(events, 2, timeout_ms);
  } while (ready < 0 && errno == EINTR);
  if ((events[0].revents & POLLIN) && AbortRequested()) {
    return FileWaitResult::kInterrupted;
  }
  if (events[0].revents & POLLIN) NormalizeAbortWake();
  if ((events[1].revents & POLLIN) && SteeringYieldRequested()) {
    return FileWaitResult::kSteering;
  }
  return std::chrono::steady_clock::now() >= deadline
             ? FileWaitResult::kTimedOut
             : FileWaitResult::kChanged;
}

#if defined(__APPLE__)
FileWaitResult WaitNative(const std::string& path, const FileStamp& observed,
                          std::chrono::steady_clock::time_point deadline) {
  Fd file(open(path.c_str(), O_EVTONLY | O_CLOEXEC));
  if (!file) return WaitFallback(deadline);
  Fd kqueue_fd(kqueue());
  if (!kqueue_fd) return WaitFallback(deadline);
  fcntl(kqueue_fd.Get(), F_SETFD, FD_CLOEXEC);

  struct kevent changes[3];
  EV_SET(&changes[0], file.Get(), EVFILT_VNODE, EV_ADD | EV_CLEAR,
         NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB, 0, nullptr);
  EV_SET(&changes[1], AbortWakeFd(), EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0,
         nullptr);
  EV_SET(&changes[2], SteeringWakeFd(), EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0,
         nullptr);
  // Register before sampling the stamp, as inotify already does on Linux;
  // otherwise a write between check and registration is never reported.
  timespec immediately = {0, 0};
  int registered;
  do {
    registered = kevent(kqueue_fd.Get(), changes, 3, nullptr, 0, &immediately);
  } while (registered < 0 && errno == EINTR);
  if (registered < 0) return WaitFallback(deadline);
  if (SnapshotFile(path) != observed) return FileWaitResult::kChanged;
  int timeout_ms = PollTimeoutMs(deadline);
  timespec timeout = {timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
  struct kevent event{};
  int ready;
  do {
    ready = kevent(kqueue_fd.Get(), nullptr, 0, &event, 1, &timeout);
  } while (ready < 0 && errno == EINTR);
  if (ready <= 0) return CurrentInterrupt();
  if (event.flags & EV_ERROR) return WaitFallback(deadline);
  if (event.ident == static_cast<uintptr_t>(AbortWakeFd())) {
    if (AbortRequested()) return FileWaitResult::kInterrupted;
    NormalizeAbortWake();
    return FileWaitResult::kChanged;
  }
  if (event.ident == static_cast<uintptr_t>(SteeringWakeFd())) {
    return SteeringYieldRequested() ? FileWaitResult::kSteering
                                    : FileWaitResult::kChanged;
  }
  return FileWaitResult::kChanged;
}
#elif defined(__linux__)
FileWaitResult WaitNative(const std::string& path, const FileStamp& observed,
                          std::chrono::steady_clock::time_point deadline) {
  Fd watcher(inotify_init1(IN_NONBLOCK | IN_CLOEXEC));
  if (!watcher) return WaitFallback(deadline);
  int watch = inotify_add_watch(
      watcher.Get(), path.c_str(),
      IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB);
  if (watch < 0) return WaitFallback(deadline);
  pollfd events[3] = {{watcher.Get(), POLLIN, 0},
                      {AbortWakeFd(), POLLIN, 0},
                      {SteeringWakeFd(), POLLIN, 0}};
  if (SnapshotFile(path) != observed) {
    inotify_rm_watch(watcher.Get(), watch);
    return FileWaitResult::kChanged;
  }
  int ready;
  do {
    ready = poll(events, 3, PollTimeoutMs(deadline));
  } while (ready < 0 && errno == EINTR);
  inotify_rm_watch(watcher.Get(), watch);
  if (ready <= 0) return CurrentInterrupt();
  if (events[1].revents & POLLIN) {
    if (AbortRequested()) return FileWaitResult::kInterrupted;
    NormalizeAbortWake();
    return FileWaitResult::kChanged;
  }
  if (events[2].revents & POLLIN) {
    return SteeringYieldRequested() ? FileWaitResult::kSteering
                                    : FileWaitResult::kChanged;
  }
  return FileWaitResult::kChanged;
}
#endif

}  // namespace

FileStamp SnapshotFile(const std::string& path) {
  struct stat state{};
  if (stat(path.c_str(), &state) != 0) return {};
  FileStamp stamp;
  stamp.device = static_cast<uint64_t>(state.st_dev);
  stamp.inode = static_cast<uint64_t>(state.st_ino);
  stamp.size = static_cast<int64_t>(state.st_size);
#if defined(__APPLE__)
  stamp.modified_seconds = state.st_mtimespec.tv_sec;
  stamp.modified_nanoseconds = state.st_mtimespec.tv_nsec;
#else
  stamp.modified_seconds = state.st_mtim.tv_sec;
  stamp.modified_nanoseconds = state.st_mtim.tv_nsec;
#endif
  return stamp;
}

FileWaitResult WaitForFileChange(
    const std::string& path, const FileStamp& observed,
    std::chrono::steady_clock::time_point deadline) {
  FileWaitResult interrupt = CurrentInterrupt();
  if (interrupt != FileWaitResult::kTimedOut) return interrupt;
  if (std::chrono::steady_clock::now() >= deadline) {
    return FileWaitResult::kTimedOut;
  }
  if (path.empty()) return WaitFallback(deadline);
#if defined(__APPLE__) || defined(__linux__)
  return WaitNative(path, observed, deadline);
#else
  return WaitFallback(deadline);
#endif
}

}  // namespace uagent
