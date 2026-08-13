// Copyright 2026 Timon Gentzsch

#include "include/core/env.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "include/core/signals.h"
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

bool OneOf(std::string_view value,
           std::initializer_list<std::string_view> allowed) {
  return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
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

int64_t AutoCompactPct() { return EnvLong("UAGENT_AUTO_COMPACT_PCT", 85); }

int64_t AutoCompactTokens() {
  return EnvBounded("UAGENT_AUTO_COMPACT_TOKENS", 0, 0);
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

bool AdaptiveSystemEnabled() {
  return EnvStr("UAGENT_ADAPT_SYSTEM", "0") != "0";
}

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
  return EnvBounded("UAGENT_MEMORY_IDLE_SECONDS", 6 * 60 * 60, 0, 48 * 60 * 60);
}

int64_t MemoryExtractBytes() {
  return EnvBounded("UAGENT_MEMORY_EXTRACT_BYTES", 32 * 1024, 4096, 256 * 1024);
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

namespace {

constexpr int64_t kAnyMin = std::numeric_limits<int64_t>::min();
constexpr int64_t kAnyMax = std::numeric_limits<int64_t>::max();

struct LongOption {
  const char* env;
  const char* name;
  int64_t RuntimeConfig::* field;
  int64_t minimum;
  int64_t maximum;
};
struct StringOption {
  const char* env;
  const char* name;
  std::string RuntimeConfig::* field;
  const char* default_value;
};
struct BoolOption {
  const char* env;
  const char* name;
  bool RuntimeConfig::* field;
  bool default_value;
};

constexpr LongOption kLongOptions[] = {
    {"UAGENT_FIRST_EVENT_TIMEOUT", "first_event_timeout_s",
     &RuntimeConfig::first_event_timeout_s, kAnyMin, kAnyMax},
    {"UAGENT_STREAM_IDLE_TIMEOUT", "stream_idle_timeout_s",
     &RuntimeConfig::stream_idle_timeout_s, kAnyMin, kAnyMax},
    {"UAGENT_REQUEST_TIMEOUT", "request_timeout_s",
     &RuntimeConfig::request_timeout_s, kAnyMin, kAnyMax},
    {"UAGENT_REQUEST_BYTES", "request_bytes", &RuntimeConfig::request_bytes,
     1024, kAnyMax},
    {"UAGENT_RESPONSE_BYTES", "response_bytes", &RuntimeConfig::response_bytes,
     kAnyMin, kAnyMax},
    {"UAGENT_MAX_STEPS", "max_steps", &RuntimeConfig::max_steps, 1, kAnyMax},
    {"UAGENT_MAX_TOOL_CALLS", "max_tool_calls", &RuntimeConfig::max_tool_calls,
     0, kAnyMax},
    {"UAGENT_MAX_TURN_SECONDS", "max_turn_seconds",
     &RuntimeConfig::max_turn_seconds, 1, kAnyMax},
    {"UAGENT_TOOL_TIMEOUT", "tool_timeout_s", &RuntimeConfig::tool_timeout_s, 0,
     kAnyMax},
    {"UAGENT_WEB_SEARCH_TIMEOUT", "web_search_timeout_s",
     &RuntimeConfig::web_search_timeout_s, 1, kAnyMax},
    {"UAGENT_WEB_SEARCH_MAX_TOKENS", "web_search_max_tokens",
     &RuntimeConfig::web_search_max_tokens, 128, kAnyMax},
    {"UAGENT_WEB_SEARCH_CALLS", "web_search_calls",
     &RuntimeConfig::web_search_calls, 1, kAnyMax},
    {"UAGENT_WEB_SEARCH_MAX_RESULTS", "web_search_max_results",
     &RuntimeConfig::web_search_max_results, 1, 25},
    {"UAGENT_WEB_SEARCH_MAX_USES", "web_search_max_uses",
     &RuntimeConfig::web_search_max_uses, 1, 30},
    {"UAGENT_MCP_TIMEOUT", "mcp_timeout_s", &RuntimeConfig::mcp_timeout_s, 1,
     kAnyMax},
    {"UAGENT_MCP_SERVERS", "mcp_servers", &RuntimeConfig::mcp_servers, 1,
     kMcpMax},
    {"UAGENT_MCP_PAGES", "mcp_pages", &RuntimeConfig::mcp_pages, 1, kAnyMax},
    {"UAGENT_MCP_TOOLS", "mcp_tools", &RuntimeConfig::mcp_tools, 1, kAnyMax},
    {"UAGENT_MCP_CONFIG_BYTES", "mcp_config_bytes",
     &RuntimeConfig::mcp_config_bytes, 1024, kAnyMax},
    {"UAGENT_MCP_RESPONSE_BYTES", "mcp_response_bytes",
     &RuntimeConfig::mcp_response_bytes, 1024, kAnyMax},
    {"UAGENT_MCP_SCHEMA_BYTES", "mcp_schema_bytes",
     &RuntimeConfig::mcp_schema_bytes, 1024, kAnyMax},
    {"UAGENT_MCP_LOG_BYTES", "mcp_log_bytes", &RuntimeConfig::mcp_log_bytes,
     1024, kAnyMax},
    {"UAGENT_PROJECT_DOC_BYTES", "project_doc_bytes",
     &RuntimeConfig::project_doc_bytes, 0, kAnyMax},
    {"UAGENT_SESSION_ARCHIVE_BYTES", "session_archive_bytes",
     &RuntimeConfig::session_archive_bytes, 0, kAnyMax},
};
constexpr StringOption kStringOptions[] = {
    {"UAGENT_OPENROUTER_PROVIDER", "openrouter_provider",
     &RuntimeConfig::openrouter_provider, ""},
    {"UAGENT_OPENROUTER_VARIANT", "openrouter_variant",
     &RuntimeConfig::openrouter_variant, ""},
    {"UAGENT_WEB_SEARCH_BACKEND", "web_search_backend",
     &RuntimeConfig::web_search_backend, "auto"},
    {"UAGENT_WEB_SEARCH_URL", "web_search_url", &RuntimeConfig::web_search_url,
     ""},
    {"UAGENT_WEB_SEARCH_EFFORT", "web_search_effort",
     &RuntimeConfig::web_search_effort, ""},
    {"UAGENT_WEB_SEARCH_MODEL", "web_search_model",
     &RuntimeConfig::web_search_model, ""},
    {"UAGENT_WEB_SEARCH_ENGINE", "web_search_engine",
     &RuntimeConfig::web_search_engine, "auto"},
    {"UAGENT_WEB_SEARCH_CONTEXT_SIZE", "web_search_context_size",
     &RuntimeConfig::web_search_context_size, ""},
    {"UAGENT_IMAGE_MODEL", "image_model", &RuntimeConfig::image_model, ""},
    {"UAGENT_MCP_ROOTS", "mcp_roots", &RuntimeConfig::mcp_roots, ""},
};
constexpr BoolOption kBoolOptions[] = {
    {"UAGENT_OPENROUTER_FALLBACKS", "openrouter_fallbacks",
     &RuntimeConfig::openrouter_fallbacks, true},
    {"UAGENT_WEB_SEARCH_SERVER", "web_search_server",
     &RuntimeConfig::web_search_server, true},
    {"UAGENT_MEMORY", "memory_enabled", &RuntimeConfig::memory_enabled, true},
    {"UAGENT_MEMORY_GENERATE", "memory_generate",
     &RuntimeConfig::memory_generate, true},
};

}  // namespace

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
  c.max_turn_cost = std::max(0.0, EnvDouble("UAGENT_MAX_TURN_COST", 0));
  c.session_budget = std::max(0.0, EnvDouble("UAGENT_SESSION_BUDGET", 0));
  if (!ValidOpenRouterVariant(c.openrouter_variant)) {
    c.openrouter_variant.clear();
  }
  if (!OneOf(c.web_search_backend,
             {"auto", "responses", "openrouter", "off"})) {
    c.web_search_backend = "auto";
  }
  if (!OneOf(c.web_search_engine, {"auto", "native", "exa", "firecrawl",
                                   "parallel", "perplexity"})) {
    c.web_search_engine = "auto";
  }
  if (!OneOf(c.web_search_context_size, {"low", "medium", "high"})) {
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
      {"auto_compact_pct", AutoCompactPct()},
      {"auto_compact_tokens", AutoCompactTokens()},
      {"tool_trace_protect_chars", ToolTraceProtectChars()},
      {"tool_trace_prune_min_chars", ToolTracePruneMinChars()},
  });
  return out;
}

}  // namespace uagent
