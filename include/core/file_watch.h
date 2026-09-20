// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_FILE_WATCH_H_
#define UAGENT_INCLUDE_CORE_FILE_WATCH_H_

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

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

// Wait for a detached log owned by another process. macOS and Linux use native
// file notifications; unsupported POSIX targets retain a bounded fallback.
FileWaitResult WaitForFileChange(
    const std::string& path, const FileStamp& observed,
    std::chrono::steady_clock::time_point deadline);

// Host coordination waits on several files/directories and its own shutdown
// pipe. Native targets use one kqueue/inotify instance; other POSIX targets use
// the same bounded fallback as the single-file API.
FileWaitResult WaitForAnyFileChange(
    const std::vector<std::string>& paths,
    std::chrono::steady_clock::time_point deadline, int wake_fd = -1);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_FILE_WATCH_H_
