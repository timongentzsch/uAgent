// Copyright 2026 Timon Gentzsch

#include "include/core/debug.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <utility>

#include "include/core/fs.h"

namespace uagent {

double ElapsedMs(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}

std::string UtcStamp(const char* format) {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&now, &tm);
  char out[32];
  std::strftime(out, sizeof out, format, &tm);
  return out;
}

std::string LocalStamp() {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  char out[64];
  std::strftime(out, sizeof out, "%Y-%m-%d %H:%M:%S %Z (UTC%z)", &tm);
  return out;
}

std::string LocalDay() {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  char out[48];
  std::strftime(out, sizeof out, "%Y-%m-%d %Z (UTC%z)", &tm);
  return out;
}

std::string UsageLedger() {
  return UagentDir(kBgDir) + "/usage-" + std::to_string(getpid()) + ".jsonl";
}

std::string DefaultDebugPath() {
  return UagentDir(kSessionsDir) + "/" + UtcStamp("%Y%m%dT%H%M%SZ") + "-" +
         std::to_string(getpid()) + ".jsonl";
}

DebugSink::~DebugSink() {
  if (file_) fclose(file_);
}

bool DebugSink::Start(std::string path) {
  if (path.empty() || path == "1") path = DefaultDebugPath();
  if (path.starts_with("~/")) {
    const char* home = getenv("HOME");
    if (home && *home) path.replace(0, 1, home);
  }
  std::error_code error;
  std::filesystem::path parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent, error);
  if (error) {
    error_ = error.message();
    return false;
  }
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    error_ = strerror(errno);
    return false;
  }
  if (fchmod(fd, 0600) != 0) {
    error_ = strerror(errno);
    close(fd);
    return false;
  }
  file_ = fdopen(fd, "w");
  if (!file_) {
    error_ = strerror(errno);
    close(fd);
    return false;
  }
  std::filesystem::path absolute = std::filesystem::absolute(path, error);
  path_ = error ? path : absolute.string();
  started_ = std::chrono::steady_clock::now();
  return true;
}

void DebugSink::Write(const std::string& event, json data) noexcept {
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

DebugSink& Debug() {
  static DebugSink debug;
  return debug;
}

JsonEventStream::~JsonEventStream() {
  if (file_) fclose(file_);
}

bool JsonEventStream::Start() {
  int fd = dup(STDOUT_FILENO);
  if (fd < 0) return false;
  file_ = fdopen(fd, "w");
  if (!file_) close(fd);
  return file_ != nullptr;
}

void JsonEventStream::Emit(const std::string& type, json data) noexcept {
  if (!file_) return;
  std::lock_guard<std::mutex> lock(mutex_);
  json record = {{"schema", "uagent.event.v1"},
                 {"seq", ++seq_},
                 {"time", UtcStamp()},
                 {"type", type},
                 {"data", std::move(data)}};
  std::string line = JsonDump(record);
  fwrite(line.data(), 1, line.size(), file_);
  fputc('\n', file_);
  fflush(file_);
}

JsonEventStream& Events() {
  static JsonEventStream events;
  return events;
}

void DebugLog(const std::string& event, json data) {
  if (Events().Enabled()) {
    if (event == "turn_start") {
      Events().Emit("turn.started", data);
    } else if (event == "tool_call") {
      Events().Emit("tool.call", data);
    } else if (event == "tool_result") {
      Events().Emit("tool.result", data);
    } else if (event == "turn_end") {
      Events().Emit("usage", data);
    }
  }
  if (Debug().Enabled()) Debug().Write(event, std::move(data));
}

}  // namespace uagent
