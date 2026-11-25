// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_LOGS_H_
#define UAGENT_INCLUDE_CORE_LOGS_H_
// What the agent records about its own work, and which `logs.level` writes it.

#include <string_view>

#include "include/core/fs.h"

namespace uagent {

// In the order of `kLogLevels`; each level writes what the ones before do.
enum class LogLevel { kOff, kEvents, kFull, kDebug };

enum class LogKind { kJournal, kDiagnostics, kMcpStderr, kCapture, kTrace };

// A record is the files in `folder` (under ~/.uagent; empty for the file
// beside the session record) whose names start with `prefix` and end with
// `suffix`.
struct LogRecord {
  LogKind kind;
  std::string_view folder, prefix, suffix;
  // The lowest level that writes it.
  LogLevel level;
};

inline constexpr LogRecord kLogRecords[] = {
    {LogKind::kJournal, "", "", kSessionJournalSuffix, LogLevel::kEvents},
    {LogKind::kDiagnostics, kLogsDir, "", ".jsonl", LogLevel::kEvents},
    {LogKind::kMcpStderr, kMcpDir, "", ".log", LogLevel::kEvents},
    // HTTP captures and tool exchanges.
    {LogKind::kCapture, kArtifactsDir, "http-", "", LogLevel::kFull},
    {LogKind::kCapture, kArtifactsDir, "exchange-", "", LogLevel::kFull},
    {LogKind::kTrace, kSessionsDir, "", ".jsonl", LogLevel::kDebug},
};

// Whether `kind` is written at the level `logs.level` names now.
bool Logs(LogKind kind);

}  // namespace uagent
#endif  // UAGENT_INCLUDE_CORE_LOGS_H_
