// Copyright 2026 Timon Gentzsch

#include "include/core/env.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "include/core/strings.h"

namespace uagent {

std::string EnvStr(const char* name, const std::string& dflt) {
  const char* v = getenv(name);
  return (v && *v) ? std::string(v) : dflt;
}

int64_t EnvLong(const char* name, int64_t dflt) {
  const char* v = getenv(name);
  int64_t value = 0;
  return ParseInt64(v, value) ? value : dflt;
}

double EnvDouble(const char* name, double dflt) {
  const char* v = getenv(name);
  double value = 0;
  return ParseFiniteDouble(v, value) ? value : dflt;
}

namespace {

constexpr int64_t kMaxMegabytes =
    static_cast<int64_t>(std::numeric_limits<size_t>::max() / (1024 * 1024));
constexpr int64_t kMaxMinusOne = std::numeric_limits<int64_t>::max() - 1;

int64_t EnvBounded(const char* name, int64_t dflt, int64_t minimum,
                   int64_t maximum = std::numeric_limits<int64_t>::max()) {
  return std::clamp(EnvLong(name, dflt), minimum, maximum);
}

}  // namespace

int64_t ToolResultCap() { return EnvLong("UAGENT_TOOL_RESULT_CHARS", 8000); }

int64_t ToolBatchResultCap() {
  int64_t per_result = ToolResultCap();
  if (per_result <= 0) return per_result;
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  return per_result > kMax / 2 ? kMax : per_result * 2;
}

int64_t ToolTraceProtectChars() {
  return EnvBounded("UAGENT_TOOL_TRACE_PROTECT_CHARS", 64 * 1024, 0);
}

int64_t ToolTracePruneMinChars() {
  return EnvBounded("UAGENT_TOOL_TRACE_PRUNE_MIN_CHARS", 32 * 1024, 0);
}

int64_t AutoCompactPct() { return EnvLong("UAGENT_AUTO_COMPACT_PCT", 95); }

int64_t CheckpointPct() { return EnvLong("UAGENT_CHECKPOINT_PCT", 65); }

int64_t CheckpointUrgentPct() {
  return EnvLong("UAGENT_CHECKPOINT_URGENT_PCT", 85);
}

int64_t ToolConcurrency() {
  return EnvBounded("UAGENT_TOOL_CONCURRENCY", 4, 1, kFgMax);
}

int64_t AgentDepth() { return EnvBounded("UAGENT_DEPTH", 0, 0, kMaxMinusOne); }

bool CanDelegate() {
  return AgentDepth() < EnvBounded("UAGENT_SUBAGENT_DEPTH", 2, 0);
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
  return EnvBounded("UAGENT_READ_FILE_MAX_LINES", 10000, 1);
}

int64_t ReadFileBytes() {
  return EnvBounded("UAGENT_READ_FILE_BYTES", 32 * 1024, 1024);
}

int64_t ReadFileResultChars() {
  constexpr int64_t kHeaderAllowance = 2048;
  int64_t bytes = ReadFileBytes();
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  return bytes > kMax - kHeaderAllowance ? kMax : bytes + kHeaderAllowance;
}

int64_t EditFileBytes() {
  return EnvLong("UAGENT_EDIT_FILE_BYTES", 10 * 1024 * 1024);
}

int64_t ListDirEntries() {
  return EnvBounded("UAGENT_LIST_DIR_ENTRIES", 1000, 1);
}

int64_t ListDirScanEntries() {
  return EnvBounded("UAGENT_LIST_DIR_SCAN_ENTRIES", 100000, 1);
}

int64_t MemoryBytes() { return EnvBounded("UAGENT_MEMORY_BYTES", 2048, 256); }

int64_t MaxMemories() { return EnvBounded("UAGENT_MEMORY_FILES", 32, 1); }

int64_t MemoryIdleSeconds() {
  return EnvBounded("UAGENT_MEMORY_IDLE_SECONDS", 6 * 60 * 60, 0);
}

int64_t MemorySessionAgeDays() {
  return EnvBounded("UAGENT_MEMORY_SESSION_DAYS", 10, 1, 365);
}

int64_t MemorySessionBytes() {
  return EnvBounded("UAGENT_MEMORY_SESSION_BYTES", 64 * 1024, 4096,
                    1024 * 1024);
}

int64_t MemoryAlwaysBytes() {
  return EnvBounded("UAGENT_MEMORY_ALWAYS_BYTES", 2048, 0, 64 * 1024);
}

int64_t SkillBodyBytes() {
  return EnvBounded("UAGENT_SKILL_BYTES", 512 * 1024, 1024, 1024 * 1024);
}

int64_t SkillDescriptionBytes() {
  return EnvBounded("UAGENT_SKILL_DESC_BYTES", 1024, 16);
}

int64_t MaxSkills() { return EnvBounded("UAGENT_SKILLS", 64, 1); }

int64_t GrepResults() {
  return EnvBounded("UAGENT_GREP_RESULTS", 200, 1, kMaxMinusOne);
}

int64_t GrepBytes() {
  return EnvBounded("UAGENT_GREP_BYTES", ToolResultCap(), 1024);
}

int64_t BashLogBytes() {
  return EnvBounded("UAGENT_BASH_LOG_BYTES", 64 * 1024 * 1024, 1024);
}

int64_t MaxBackgroundJobs() {
  return EnvBounded("UAGENT_MAX_BACKGROUND_JOBS", 8, 1, kBgMax);
}

int64_t CheckpointFileLines() {
  return EnvLong("UAGENT_CHECKPOINT_FILE_LINES", 120);
}

int64_t McpConfigBytes() {
  return EnvBounded("UAGENT_MCP_CONFIG_BYTES", 1024 * 1024, 1024);
}

int64_t McpDescriptionChars() { return EnvLong("UAGENT_MCP_DESC_CHARS", 400); }

int64_t MaxPendingAttachments() {
  return EnvBounded("UAGENT_PENDING_ATTACHMENTS", 8, 1);
}

int64_t AttachmentLimitMb() {
  return EnvBounded("UAGENT_ATTACHMENT_MB", 10, 1, kMaxMegabytes);
}

int64_t TerminalImageLimitMb() {
  return EnvBounded("UAGENT_TERMINAL_IMAGE_MB", 10, 1, kMaxMegabytes);
}

int64_t ImageMaxColumns() {
  return EnvBounded("UAGENT_IMAGE_MAX_COLUMNS", 200, 1);
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
  return EnvBounded("UAGENT_TERMINAL_DAYS", 7, 0);
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
  if (!ValidOpenRouterVariant(c.openrouter_variant)) {
    c.openrouter_variant.clear();
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
      {"tool_trace_protect_chars", ToolTraceProtectChars()},
      {"tool_trace_prune_min_chars", ToolTracePruneMinChars()},
  });
  return out;
}

}  // namespace uagent
