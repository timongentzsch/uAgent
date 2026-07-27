// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_DEBUG_H_
#define UAGENT_INCLUDE_CORE_DEBUG_H_
// Opt-in JSONL tracing and the timestamps it records. Traces can contain
// private source and model reasoning, so they are off unless asked for.

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>

#include "include/core/fs.h"
#include "include/core/json.h"

namespace uagent {

inline double ElapsedMs(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}

inline std::string UtcStamp(const char* format = "%Y-%m-%dT%H:%M:%SZ") {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&now, &tm);
  char out[32];
  std::strftime(out, sizeof out, format, &tm);
  return out;
}

inline std::string LocalStamp() {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  char out[64];
  std::strftime(out, sizeof out, "%Y-%m-%d %H:%M:%S %Z (UTC%z)", &tm);
  return out;
}

// Subagents are separate processes, so they append what they spent here for
// this session to fold into its own totals. Named by the parent's pid; pruned
// with the other background files.
inline std::string UsageLedger() {
  return UagentDir(kBgDir) + "/usage-" + std::to_string(getpid()) + ".jsonl";
}

inline std::string DefaultDebugPath() {
  return UagentDir(kSessionsDir) + "/" + UtcStamp("%Y%m%dT%H%M%SZ") + "-" +
         std::to_string(getpid()) + ".jsonl";
}

class DebugLog {
 public:
  ~DebugLog() {
    if (file_) fclose(file_);
  }

  bool Start(std::string path = "") {
    if (path.empty() || path == "1") path = DefaultDebugPath();
    if (path.starts_with("~/")) {
      const char* home = getenv("HOME");
      if (home && *home) path.replace(0, 1, home);
    }
    std::error_code ec;
    std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    if (ec) {
      error_ = ec.message();
      return false;
    }
    int fd =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
      error_ = strerror(errno);
      return false;
    }
    if (fchmod(fd, 0600) != 0) {
      error_ = strerror(errno);
      ::close(fd);
      return false;
    }
    file_ = fdopen(fd, "w");
    if (!file_) {
      error_ = strerror(errno);
      ::close(fd);
      return false;
    }
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    path_ = ec ? path : absolute.string();
    started_ = std::chrono::steady_clock::now();
    return true;
  }

  bool Enabled() const { return file_ != nullptr; }
  const std::string& Path() const { return path_; }
  const std::string& Error() const { return error_; }

  void Write(const std::string& event, json data = json::object()) noexcept {
    if (!file_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    json record = {{"seq", ++seq_},
                   {"time", UtcStamp()},
                   {"elapsed_ms", ElapsedMs(started_)},
                   {"event", event},
                   {"data", std::move(data)}};
    std::string line = JsonDump(record);
    fwrite(line.data(), 1, line.size(), file_);
    fputc('\n', file_);
    fflush(file_);
  }

 private:
  FILE* file_ = nullptr;
  std::mutex mutex_;
  std::chrono::steady_clock::time_point started_;
  std::string path_, error_;
  int64_t seq_ = 0;
};

inline DebugLog g_debug;

// compact debug logging helper — guards the enabled() check + alloc avoidance
inline void DebugLog(const std::string& event, json data = json::object()) {
  if (g_debug.Enabled()) g_debug.Write(event, std::move(data));
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_DEBUG_H_
