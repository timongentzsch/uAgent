// Copyright 2026 Timon Gentzsch

#include "include/core/env.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace uagent {

std::string EnvStr(const char* name, const std::string& dflt) {
  const char* v = getenv(name);
  return (v && *v) ? std::string(v) : dflt;
}

int64_t EnvLong(const char* name, int64_t dflt) {
  const char* v = getenv(name);
  if (!v || !*v) return dflt;
  char* end = nullptr;
  int64_t n = strtol(v, &end, 10);
  return (end && *end == '\0') ? n : dflt;
}

double EnvDouble(const char* name, double dflt) {
  const char* v = getenv(name);
  if (!v || !*v) return dflt;
  char* end = nullptr;
  errno = 0;
  double n = strtod(v, &end);
  return !errno && end && *end == '\0' ? n : dflt;
}

int64_t ToolResultCap() { return EnvLong("UAGENT_TOOL_RESULT_CHARS", 8000); }

int64_t ToolBatchResultCap() {
  int64_t per_result = ToolResultCap();
  if (per_result <= 0) return per_result;
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  return per_result > kMax / 2 ? kMax : per_result * 2;
}

int64_t AutoCompactPct() { return EnvLong("UAGENT_AUTO_COMPACT_PCT", 95); }

int64_t CheckpointPct() { return EnvLong("UAGENT_CHECKPOINT_PCT", 65); }

int64_t CheckpointUrgentPct() {
  return EnvLong("UAGENT_CHECKPOINT_URGENT_PCT", 85);
}

int64_t ToolConcurrency() {
  return std::clamp(EnvLong("UAGENT_TOOL_CONCURRENCY", 4), int64_t{1},
                    static_cast<int64_t>(kFgMax));
}

int64_t AgentDepth() {
  return std::max(int64_t{0}, EnvLong("UAGENT_DEPTH", 0));
}

bool CanDelegate() {
  return AgentDepth() <
         std::max(int64_t{0}, EnvLong("UAGENT_SUBAGENT_DEPTH", 2));
}

bool LeanToolset() { return EnvStr("UAGENT_TOOLSET") == "lean"; }

int64_t SubagentMaxSteps() { return EnvLong("UAGENT_SUBAGENT_MAX_STEPS", 25); }

int64_t SubagentMaxToolCalls() {
  return EnvLong("UAGENT_SUBAGENT_MAX_TOOL_CALLS", 60);
}

std::string TaskModel() { return EnvStr("UAGENT_TASK_MODEL"); }

int64_t MaxOutputTokens() { return EnvLong("UAGENT_MAX_TOKENS", 16000); }

bool SteeringEnabled() { return EnvStr("UAGENT_STEERING", "1") != "0"; }

int64_t ReadFileLines() { return EnvLong("UAGENT_READ_FILE_LINES", 1000); }

int64_t ReadFileMaxLines() {
  return std::max(int64_t{1}, EnvLong("UAGENT_READ_FILE_MAX_LINES", 10000));
}

int64_t ReadFileBytes() {
  return std::max(int64_t{1024}, EnvLong("UAGENT_READ_FILE_BYTES", 32 * 1024));
}

int64_t ReadFileResultChars() {
  constexpr int64_t kHeaderAllowance = 2048;
  int64_t bytes = ReadFileBytes();
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  return bytes > kMax - kHeaderAllowance ? kMax : bytes + kHeaderAllowance;
}

bool ReadFileCountsTotal() {
  return EnvLong("UAGENT_READ_FILE_COUNT_TOTAL", 0) != 0;
}

int64_t EditFileBytes() {
  return EnvLong("UAGENT_EDIT_FILE_BYTES", 10 * 1024 * 1024);
}

int64_t ListDirEntries() { return EnvLong("UAGENT_LIST_DIR_ENTRIES", 1000); }

int64_t ListDirScanEntries() {
  return std::max(int64_t{1}, EnvLong("UAGENT_LIST_DIR_SCAN_ENTRIES", 100000));
}

int64_t MemoryBytes() {
  return std::max(int64_t{256}, EnvLong("UAGENT_MEMORY_BYTES", 2048));
}

int64_t MaxMemories() {
  return std::max(int64_t{1}, EnvLong("UAGENT_MEMORY_FILES", 32));
}

int64_t SkillBodyBytes() {
  return std::max(int64_t{1024}, EnvLong("UAGENT_SKILL_BYTES", 16 * 1024));
}

int64_t SkillDescriptionBytes() {
  return std::max(int64_t{16}, EnvLong("UAGENT_SKILL_DESC_BYTES", 512));
}

int64_t MaxSkills() {
  return std::max(int64_t{1}, EnvLong("UAGENT_SKILLS", 64));
}

int64_t GrepResults() {
  return std::max(int64_t{1}, EnvLong("UAGENT_GREP_RESULTS", 200));
}

int64_t GrepBytes() {
  return std::max(int64_t{1024}, EnvLong("UAGENT_GREP_BYTES", ToolResultCap()));
}

int64_t BashLogBytes() {
  return std::max(int64_t{1024},
                  EnvLong("UAGENT_BASH_LOG_BYTES", 64 * 1024 * 1024));
}

int64_t MaxBackgroundJobs() {
  return std::clamp(EnvLong("UAGENT_MAX_BACKGROUND_JOBS", 8), int64_t{1},
                    static_cast<int64_t>(kBgMax));
}

int64_t CheckpointFileLines() {
  return EnvLong("UAGENT_CHECKPOINT_FILE_LINES", 120);
}

int64_t McpConfigBytes() {
  return std::max(int64_t{1024},
                  EnvLong("UAGENT_MCP_CONFIG_BYTES", 1024 * 1024));
}

int64_t McpDescriptionChars() { return EnvLong("UAGENT_MCP_DESC_CHARS", 400); }

int64_t MaxPendingAttachments() {
  return std::max(int64_t{1}, EnvLong("UAGENT_PENDING_ATTACHMENTS", 8));
}

int64_t AttachmentLimitMb() {
  return std::max(int64_t{1}, EnvLong("UAGENT_ATTACHMENT_MB", 10));
}

int64_t TerminalImageLimitMb() {
  return std::max(int64_t{1}, EnvLong("UAGENT_TERMINAL_IMAGE_MB", 10));
}

int64_t ImageMaxColumns() {
  return std::max(int64_t{1}, EnvLong("UAGENT_IMAGE_MAX_COLUMNS", 200));
}

int64_t ImageColumns(int64_t available) {
  return EnvLong("UAGENT_IMAGE_COLUMNS", available);
}

int64_t ContextWindow() { return EnvLong("UAGENT_CONTEXT", 0); }

int64_t HistoryDays() { return EnvLong("UAGENT_HISTORY_DAYS", 30); }

int64_t HistoryFiles() { return EnvLong("UAGENT_HISTORY_FILES", 200); }

int64_t DebugDays() { return EnvLong("UAGENT_DEBUG_DAYS", 14); }

int64_t DebugFiles() { return EnvLong("UAGENT_DEBUG_FILES", 50); }

int64_t BgDays() { return EnvLong("UAGENT_BG_DAYS", 7); }

int64_t BgFiles() { return EnvLong("UAGENT_BG_FILES", 200); }

int64_t McpLogDays() { return EnvLong("UAGENT_MCP_LOG_DAYS", 7); }

int64_t McpLogFiles() { return EnvLong("UAGENT_MCP_LOG_FILES", 100); }

int64_t TerminalRecordDays() {
  return std::max(int64_t{0}, EnvLong("UAGENT_TERMINAL_DAYS", 7));
}

RuntimeConfig RuntimeConfig::FromEnvironment() {
  RuntimeConfig c;
  for (const LongOption& option : kLongOptions) {
    c.*option.field = std::clamp(EnvLong(option.env, c.*option.field),
                                 option.minimum, option.maximum);
  }
  for (const StringOption& option : kStringOptions) {
    c.*option.field = EnvStr(option.env, option.default_value);
  }
  c.web_search_api_key = EnvStr("UAGENT_WEB_SEARCH_API_KEY");
  for (const BoolOption& option : kBoolOptions) {
    c.*option.field =
        EnvStr(option.env, option.default_value ? "1" : "0") != "0";
  }
  c.max_turn_cost = EnvDouble("UAGENT_MAX_TURN_COST", 1.0);
  c.session_budget = std::max(0.0, EnvDouble("UAGENT_SESSION_BUDGET", 0));
  if (c.checkpoint_mode != "off" && c.checkpoint_mode != "shadow" &&
      c.checkpoint_mode != "apply") {
    c.checkpoint_mode = "apply";
  }
  if (c.web_search_backend != "auto" && c.web_search_backend != "responses" &&
      c.web_search_backend != "openrouter" && c.web_search_backend != "off") {
    c.web_search_backend = "auto";
  }
  const std::vector<std::string> search_engines = {
      "auto", "native", "exa", "firecrawl", "parallel", "perplexity"};
  if (std::find(search_engines.begin(), search_engines.end(),
                c.web_search_engine) == search_engines.end()) {
    c.web_search_engine = "auto";
  }
  if (c.web_search_context_size != "low" &&
      c.web_search_context_size != "medium" &&
      c.web_search_context_size != "high") {
    c.web_search_context_size.clear();
  }
  return c;
}

json RuntimeConfig::DiagnosticJson() const {
  json out;
  for (const LongOption& option : kLongOptions) {
    out[option.name] = this->*option.field;
  }
  for (const StringOption& option : kStringOptions) {
    out[option.name] = this->*option.field;
  }
  for (const BoolOption& option : kBoolOptions) {
    out[option.name] = this->*option.field;
  }
  out.update({
      {"max_turn_cost", max_turn_cost},
      {"session_budget", session_budget},
      {"checkpoint_pct", CheckpointPct()},
      {"checkpoint_urgent_pct", CheckpointUrgentPct()},
      {"auto_compact_pct", AutoCompactPct()},
  });
  return out;
}

}  // namespace uagent
