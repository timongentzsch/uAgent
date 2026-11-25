// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_LOGS_H_
#define UAGENT_INCLUDE_CORE_LOGS_H_
// What the agent records about its own work, and which `logs.level` writes it.

#include <string_view>

#include "include/core/fs.h"

namespace uagent {

// In the order of `kLogLevels`; each level writes what the ones before do.
enum class LogLevel { kOff, kEvents, kFull, kDebug };

enum class LogKind {
  kJournal,
  kMcpStderr,
  kCapture,
  kExchange,
  kTrace,
  kToolOutput,
  kCommandLog,
};

struct LogRecord {
  LogKind kind;
  // The folder under ~/.uagent, empty for a file beside the session record.
  std::string_view folder;
  // The lowest level that writes it; off for records tools need to work.
  LogLevel level;
};

inline constexpr LogRecord kLogRecords[] = {
    {LogKind::kJournal, "", LogLevel::kEvents},
    {LogKind::kMcpStderr, kMcpDir, LogLevel::kEvents},
    {LogKind::kCapture, kArtifactsDir, LogLevel::kFull},
    {LogKind::kExchange, kArtifactsDir, LogLevel::kFull},
    {LogKind::kTrace, kSessionsDir, LogLevel::kDebug},
    {LogKind::kToolOutput, kArtifactsDir, LogLevel::kOff},
    {LogKind::kCommandLog, kActivityDir, LogLevel::kOff},
};

// Whether `kind` is written at the level `logs.level` names now.
bool Logs(LogKind kind);

}  // namespace uagent
#endif  // UAGENT_INCLUDE_CORE_LOGS_H_
