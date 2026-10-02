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
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <sys/event.h>
#elif defined(__linux__)
#include <sys/inotify.h>
#endif

#include "include/core/fd.h"
#include "include/core/fs.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/time.h"

namespace uagent {
std::string DocumentRevision(const std::string& path, const std::string& body) {
  const auto stamp = SnapshotFile(path);
  return HashHex(body + std::to_string(stamp.inode) + ":" +
                 std::to_string(stamp.modified_seconds) + ":" +
                 std::to_string(stamp.modified_nanoseconds));
}
namespace {

FileWaitResult CurrentInterrupt() {
  if (AbortRequested()) return FileWaitResult::kInterrupted;
  if (SteeringYieldRequested()) return FileWaitResult::kSteering;
  return FileWaitResult::kTimedOut;
}

FileWaitResult WaitFallback(std::chrono::steady_clock::time_point deadline) {
  pollfd events[2] = {{AbortWakeFd(), POLLIN, 0},
                      {SteeringWakeFd(), POLLIN, 0}};
  PollRetry(events, 2, std::min(100, PollTimeoutMs(deadline)));
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

FileWaitResult WaitHostFallback(std::chrono::steady_clock::time_point deadline,
                                int wake_fd) {
  pollfd wake{wake_fd, POLLIN, 0};
  const int ready =
      PollRetry(wake_fd >= 0 ? &wake : nullptr, wake_fd >= 0 ? 1 : 0,
                std::min(5000, PollTimeoutMs(deadline)));
  if (ready > 0 && (wake.revents & POLLIN)) {
    return FileWaitResult::kInterrupted;
  }
  return std::chrono::steady_clock::now() >= deadline
             ? FileWaitResult::kTimedOut
             : FileWaitResult::kChanged;
}

std::vector<std::string> WatchTargets(const std::vector<std::string>& paths) {
  std::vector<std::string> targets;
  targets.reserve(paths.size());
  for (const std::string& path : paths) {
    std::filesystem::path target(path);
    while (!target.empty() && !PathExists(target.string())) {
      target = target.parent_path();
    }
    if (!target.empty()) targets.push_back(target.string());
  }
  std::sort(targets.begin(), targets.end());
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
  return targets;
}

#if defined(__APPLE__)
int KeventRetry(int queue, const struct kevent* changes, int count,
                struct kevent* event, const timespec* timeout) {
  int ready;
  do {
    ready = kevent(queue, changes, count, event, event ? 1 : 0, timeout);
  } while (ready < 0 && errno == EINTR);
  return ready;
}
#endif

}  // namespace

bool NativeWatch::Open() {
  if (queue_) return true;
#if defined(__APPLE__)
  queue_.Reset(kqueue());
  if (queue_) fcntl(queue_.Get(), F_SETFD, FD_CLOEXEC);
#elif defined(__linux__)
  queue_.Reset(inotify_init1(IN_NONBLOCK | IN_CLOEXEC));
#endif
  return static_cast<bool>(queue_);
}

bool NativeWatch::Watch(const std::string& path, Events events) {
  if (!Open()) return false;
#if defined(__APPLE__)
  Fd file(open(path.c_str(), O_EVTONLY | O_CLOEXEC));
  if (!file) return false;
  const uint32_t notes = events == Events::kArrivals
                             ? NOTE_WRITE
                             : NOTE_WRITE | NOTE_DELETE | NOTE_RENAME |
                                   NOTE_ATTRIB |
                                   (events == Events::kTree ? NOTE_EXTEND : 0);
  struct kevent change;
  EV_SET(&change, file.Get(), EVFILT_VNODE, EV_ADD | EV_CLEAR, notes, 0,
         nullptr);
  if (KeventRetry(queue_.Get(), &change, 1, nullptr, nullptr) < 0) {
    return false;
  }
  watched_.push_back(std::move(file));
  return true;
#elif defined(__linux__)
  uint32_t mask = IN_MOVED_TO | IN_CLOSE_WRITE;
  if (events != Events::kArrivals) {
    mask =
        IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB;
  }
  if (events == Events::kTree) {
    mask |= IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO;
  }
  return inotify_add_watch(queue_.Get(), path.c_str(), mask) >= 0;
#else
  return false;
#endif
}

bool NativeWatch::Wake(int fd) {
  if (fd < 0) return true;
  if (!Open()) return false;
#if defined(__APPLE__)
  struct kevent change;
  EV_SET(&change, fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr);
  if (KeventRetry(queue_.Get(), &change, 1, nullptr, nullptr) < 0) {
    return false;
  }
#endif
  wakes_.push_back(fd);
  return true;
}

int NativeWatch::Wait(std::chrono::steady_clock::time_point deadline) const {
  if (!queue_) return -1;
#if defined(__APPLE__)
  const int timeout_ms = PollTimeoutMs(deadline);
  const timespec timeout = {timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
  struct kevent event{};
  if (KeventRetry(queue_.Get(), nullptr, 0, &event, &timeout) <= 0) return -1;
  for (int wake : wakes_) {
    if (!(event.flags & EV_ERROR) &&
        event.ident == static_cast<uintptr_t>(wake)) {
      return wake;
    }
  }
  return queue_.Get();
#elif defined(__linux__)
  std::vector<pollfd> events = {{queue_.Get(), POLLIN, 0}};
  for (int wake : wakes_) events.push_back({wake, POLLIN, 0});
  if (PollRetry(events.data(), static_cast<nfds_t>(events.size()),
                PollTimeoutMs(deadline)) <= 0) {
    return -1;
  }
  for (size_t i = 1; i < events.size(); ++i) {
    if (events[i].revents & POLLIN) return events[i].fd;
  }
  return queue_.Get();
#else
  return -1;
#endif
}

void NativeWatch::Drain() const {
  if (!queue_) return;
#if defined(__APPLE__)
  struct kevent event;
  const timespec immediately = {0, 0};
  while (kevent(queue_.Get(), nullptr, 0, &event, 1, &immediately) > 0) {
  }
#elif defined(__linux__)
  char buffer[4096];
  while (read(queue_.Get(), buffer, sizeof buffer) > 0) {
  }
#endif
}

FileWaitResult WaitForAnyFileChange(
    const std::vector<std::string>& paths,
    std::chrono::steady_clock::time_point deadline, int wake_fd,
    const std::map<std::string, FileStamp>& prior) {
  if (std::chrono::steady_clock::now() >= deadline) {
    return FileWaitResult::kTimedOut;
  }
  std::vector<FileStamp> observed;
  observed.reserve(paths.size());
  for (const std::string& path : paths) {
    const auto found = prior.find(path);
    observed.push_back(found == prior.end() ? SnapshotFile(path)
                                            : found->second);
  }
  const std::vector<std::string> targets = WatchTargets(paths);
  // Preserve enough descriptors for the HTTP/runtime paths under conservative
  // process limits. The fallback still observes the host wake and rechecks.
#if defined(__APPLE__)
  constexpr bool kEveryTarget = true;
  if (targets.size() > kKqueueWatchTargets) {
    return WaitHostFallback(deadline, wake_fd);
  }
#else
  constexpr bool kEveryTarget = false;
#endif
  NativeWatch watch;
  size_t watched = 0;
  for (const std::string& target : targets) {
    if (watch.Watch(target, NativeWatch::Events::kTree)) {
      ++watched;
    } else if (kEveryTarget) {
      break;
    }
  }
  if (!watched || (kEveryTarget && watched != targets.size()) ||
      !watch.Wake(wake_fd)) {
    return WaitHostFallback(deadline, wake_fd);
  }
  for (size_t i = 0; i < paths.size(); ++i) {
    if (SnapshotFile(paths[i]) != observed[i]) return FileWaitResult::kChanged;
  }
  const int ready = watch.Wait(deadline);
  if (ready < 0) return FileWaitResult::kTimedOut;
  return ready == wake_fd ? FileWaitResult::kInterrupted
                          : FileWaitResult::kChanged;
}

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
  // Watch before sampling the stamp: a write between the check and the watch
  // would otherwise never be reported.
  NativeWatch watch;
  if (!watch.Watch(path, NativeWatch::Events::kFile) ||
      !watch.Wake(AbortWakeFd()) || !watch.Wake(SteeringWakeFd())) {
    return WaitFallback(deadline);
  }
  if (SnapshotFile(path) != observed) return FileWaitResult::kChanged;
  const int ready = watch.Wait(deadline);
  if (ready < 0) return CurrentInterrupt();
  if (ready == AbortWakeFd()) {
    if (AbortRequested()) return FileWaitResult::kInterrupted;
    NormalizeAbortWake();
    return FileWaitResult::kChanged;
  }
  if (ready == SteeringWakeFd()) {
    return SteeringYieldRequested() ? FileWaitResult::kSteering
                                    : FileWaitResult::kChanged;
  }
  return FileWaitResult::kChanged;
}

}  // namespace uagent
