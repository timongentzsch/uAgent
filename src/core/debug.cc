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

#include "include/core/events.h"
#include "include/core/fs.h"

namespace uagent {
namespace {

void WriteJsonLine(FILE* file, const json& record) {
  std::string line = JsonDump(record);
  fwrite(line.data(), 1, line.size(), file);
  fputc('\n', file);
  fflush(file);
}

std::string Stamp(bool utc, const char* format) {
  std::time_t now = std::time(nullptr);
  std::tm broken{};
  if (utc) {
    gmtime_r(&now, &broken);
  } else {
    localtime_r(&now, &broken);
  }
  char out[64];
  std::strftime(out, sizeof out, format, &broken);
  return out;
}

std::string DefaultDebugPath() {
  return UagentDir(kSessionsDir) + "/" + UtcStamp("%Y%m%dT%H%M%SZ") + "-" +
         std::to_string(getpid()) + ".jsonl";
}

}  // namespace

double ElapsedMs(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}

std::string UtcStamp(const char* format) { return Stamp(/*utc=*/true, format); }

std::string LocalStamp() {
  return Stamp(/*utc=*/false, "%Y-%m-%d %H:%M:%S %Z (UTC%z)");
}

std::string LocalDay() { return Stamp(/*utc=*/false, "%Y-%m-%d %Z (UTC%z)"); }

std::string UsageLedger() {
  return UagentDir(kBgDir) + "/usage-" + std::to_string(getpid()) + ".jsonl";
}

DebugSink::~DebugSink() { Stop(); }

bool DebugSink::Start(std::string path) {
  if (file_) return true;
  stopping_ = false;
  writing_ = false;
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
  writer_ = std::thread([this] { Run(); });
  return true;
}

void DebugSink::Flush() {
  if (!file_) return;
  std::unique_lock<std::mutex> lock(mutex_);
  wake_.wait(lock, [this] { return queue_.empty() && !writing_; });
}

void DebugSink::Stop() {
  if (!file_) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (writer_.joinable()) writer_.join();
  fclose(file_);
  file_ = nullptr;
}

void DebugSink::Write(const std::string& event, json data) noexcept {
  if (!file_) return;
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.push_back({{"seq", ++seq_},
                    {"time", UtcStamp()},
                    {"elapsed_ms", ElapsedMs(started_)},
                    {"event", event},
                    {"data", std::move(data)}});
  wake_.notify_one();
}

void DebugSink::Run() {
  for (;;) {
    json record;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        if (stopping_) break;
        continue;
      }
      record = std::move(queue_.front());
      queue_.pop_front();
      writing_ = true;
    }
    WriteJsonLine(file_, record);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      writing_ = false;
    }
    wake_.notify_all();
  }
}

DebugSink& Debug() {
  static DebugSink inactive;
  Observability* observability = ActiveObservability();
  return observability ? observability->DebugOutput() : inactive;
}

JsonEventStream::~JsonEventStream() { Stop(); }

bool JsonEventStream::Start() {
  if (file_) return true;
  int fd = dup(STDOUT_FILENO);
  if (fd < 0) return false;
  file_ = fdopen(fd, "w");
  if (!file_) close(fd);
  return file_ != nullptr;
}

void JsonEventStream::Stop() {
  if (!file_) return;
  fclose(file_);
  file_ = nullptr;
}

void JsonEventStream::Emit(const std::string& type, json data) noexcept {
  if (!file_) return;
  std::lock_guard<std::mutex> lock(mutex_);
  json record = {{"schema", "uagent.event.v1"},
                 {"seq", ++seq_},
                 {"time", UtcStamp()},
                 {"type", type},
                 {"data", std::move(data)}};
  WriteJsonLine(file_, record);
}

void DebugLog(const std::string& event, json data) {
  Observability* observability = ActiveObservability();
  if (observability) observability->Diagnostic(event, std::move(data));
}

}  // namespace uagent
