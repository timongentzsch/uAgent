// Copyright 2026 Timon Gentzsch

#include "include/core/logs.h"

#include <algorithm>
#include <iterator>
#include <string>

#include "include/core/config_registry.h"

namespace uagent {

bool Logs(LogKind kind) {
  const std::string name = StringSetting(Cfg("UAGENT_LOG_LEVEL"));
  const auto* named = std::ranges::find(kLogLevels, name);
  // An unknown name reads as the default, full.
  const LogLevel level = named == std::end(kLogLevels)
                             ? LogLevel::kFull
                             : static_cast<LogLevel>(named - kLogLevels);
  const auto* record = std::ranges::find(kLogRecords, kind, &LogRecord::kind);
  return level >= record->level;
}

}  // namespace uagent
