// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_DEBUG_H_
#define UAGENT_INCLUDE_CORE_DEBUG_H_
// Opt-in JSONL tracing and the timestamps it records. Traces can contain
// private source and model reasoning, so they are off unless asked for.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

#include "include/core/json.h"

namespace uagent {

double ElapsedMs(std::chrono::steady_clock::time_point start);
std::string UtcStamp(const char* format = "%Y-%m-%dT%H:%M:%SZ");
std::string LocalStamp();
std::string LocalDay();

// Subagents are separate processes, so they append what they spent here for
// this session to fold into its own totals. Named by the parent's pid; pruned
// with the other background files.
std::string UsageLedger();
std::string DefaultDebugPath();

class DebugSink {
 public:
  ~DebugSink();

  bool Start(std::string path = "");

  bool Enabled() const { return file_ != nullptr; }
  const std::string& Path() const { return path_; }
  const std::string& Error() const { return error_; }

  void Write(const std::string& event, json data = json::object()) noexcept;

 private:
  FILE* file_ = nullptr;
  std::mutex mutex_;
  std::chrono::steady_clock::time_point started_;
  std::string path_, error_;
  int64_t seq_ = 0;
};

DebugSink& Debug();

class JsonEventStream {
 public:
  ~JsonEventStream();

  bool Start();

  bool Enabled() const { return file_ != nullptr; }

  void Emit(const std::string& type, json data = json::object()) noexcept;

 private:
  FILE* file_ = nullptr;
  std::mutex mutex_;
  int64_t seq_ = 0;
};

JsonEventStream& Events();

// Compact helper that avoids constructing trace records when both sinks are
// disabled.
void DebugLog(const std::string& event, json data = json::object());

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_DEBUG_H_
