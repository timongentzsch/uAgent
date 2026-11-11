// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_FILE_WATCH_H_
#define UAGENT_INCLUDE_CORE_FILE_WATCH_H_
// Waiting for a file to change, natively where the platform can, and the
// stamps and revisions used to tell that it did.

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "include/core/fd.h"

namespace uagent {

enum class FileWaitResult { kChanged, kTimedOut, kInterrupted, kSteering };

struct FileStamp {
  uint64_t device = 0;
  uint64_t inode = 0;
  int64_t size = -1;
  int64_t modified_seconds = 0;
  int64_t modified_nanoseconds = 0;

  bool operator==(const FileStamp&) const = default;
};

FileStamp SnapshotFile(const std::string& path);
std::string DocumentRevision(const std::string& path, const std::string& body);

// Native notifications on macOS and Linux; a bounded fallback elsewhere.
FileWaitResult WaitForFileChange(
    const std::string& path, const FileStamp& observed,
    std::chrono::steady_clock::time_point deadline);

// Also returns when `wake_fd` is readable. `prior` holds the stamps the caller
// read before waiting, so a change in between is not missed.
FileWaitResult WaitForAnyFileChange(
    const std::vector<std::string>& paths,
    std::chrono::steady_clock::time_point deadline, int wake_fd = -1,
    const std::map<std::string, FileStamp>& prior = {});

// One kqueue/inotify instance watching paths and readable wake descriptors.
// Default-constructed it watches nothing and Get() is -1.
class NativeWatch {
 public:
  // A file's own changes; a directory's entries coming and going as well; or
  // only entries arriving in a directory.
  enum class Events { kFile, kTree, kArrivals };

  // False when the platform cannot watch `path`, leaving the rest in place.
  bool Watch(const std::string& path, Events events);
  // Negative descriptors are skipped.
  bool Wake(int fd);
  int Get() const { return queue_.Get(); }
  // Until `deadline`: a wake descriptor that became readable, Get() for a
  // watched change, or -1 when nothing arrived or the wait failed.
  int Wait(std::chrono::steady_clock::time_point deadline) const;
  // Consumes pending notifications without blocking.
  void Drain() const;

 private:
  bool Open();

  Fd queue_;
  // kqueue watches descriptors: each stays open as long as its watch.
  std::vector<Fd> watched_;
  std::vector<int> wakes_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_FILE_WATCH_H_
