// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_FILE_WATCH_H_
#define UAGENT_INCLUDE_CORE_FILE_WATCH_H_

#include <chrono>
#include <cstdint>
#include <string>

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

// Wait for a detached log owned by another process. macOS and Linux use native
// file notifications; unsupported POSIX targets retain a bounded fallback.
FileWaitResult WaitForFileChange(
    const std::string& path, const FileStamp& observed,
    std::chrono::steady_clock::time_point deadline);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_FILE_WATCH_H_
