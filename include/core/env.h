// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_ENV_H_
#define UAGENT_INCLUDE_CORE_ENV_H_
// Environment lookups and the parsed runtime configuration. Every tunable
// bound the agent honours is declared here with its default.

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "include/core/json.h"
#include "include/core/signals.h"

namespace uagent {

std::string EnvStr(const char* name, const std::string& dflt = "");

int64_t EnvLong(const char* name, int64_t dflt);

double EnvDouble(const char* name, double dflt);

// Accessors shared by their consumers and the session_ready diagnostic.
int64_t ToolResultCap();
int64_t ToolBatchResultCap();
int64_t ToolTraceProtectChars();
int64_t ToolTracePruneMinChars();
int64_t AutoCompactPct();
int64_t ToolConcurrency();
// Delegation depth: 0 is the interactive coordinator. A subagent may delegate
// again while it stays under the cap, so nesting is bounded, not banned.
int64_t AgentDepth();
bool CanDelegate();
bool LeanToolset();
// Budgets handed to a delegated child. Lower than the coordinator's own, so one
// flailing subagent cannot spend the whole turn.
int64_t SubagentMaxSteps();
int64_t SubagentMaxToolCalls();
std::string TaskModel();
int64_t MaxOutputTokens();
bool SteeringEnabled();

inline constexpr std::string_view kOpenRouterVariants[] = {"nitro", "floor",
                                                           "exacto"};

inline bool ValidOpenRouterVariant(std::string_view variant) {
  if (variant.empty()) return true;
  for (std::string_view candidate : kOpenRouterVariants) {
    if (variant == candidate) return true;
  }
  return false;
}

// Bounded tunables. Every UAGENT_* limit the agent honours is declared here
// with its default and its clamp, so the set can be read — and compared with
// docs/OPERATIONS.md — without hunting through the modules that apply them.
int64_t ReadFileLines();
int64_t ReadFileMaxLines();
int64_t ReadFileBytes();
int64_t ReadFileResultChars();
int64_t EditFileBytes();
int64_t ListDirEntries();
int64_t ListDirScanEntries();
int64_t MemoryBytes();
int64_t MaxMemories();
int64_t MemoryAlwaysBytes();
int64_t SkillBodyBytes();
// Descriptions stay bounded because discovery may return several at once;
// bodies are sent only when a skill is opened.
int64_t SkillDescriptionBytes();
int64_t MaxSkills();
int64_t GrepResults();
int64_t GrepBytes();
int64_t BashLogBytes();
int64_t MaxBackgroundJobs();
int64_t McpConfigBytes();
int64_t McpDescriptionChars();
int64_t MaxPendingAttachments();
int64_t AttachmentLimitMb();
// Shared by the attachment path and MCP image results, which previously each
// carried their own copy of the default.
int64_t TerminalImageLimitMb();
int64_t ImageMaxColumns();
// The fallback is the width actually available, so it is passed in.
int64_t ImageColumns(int64_t available);
int64_t ContextWindow();
// Retention for the pruned artifact trees: days kept, then newest-N kept.
int64_t HistoryDays();
int64_t HistoryFiles();
int64_t DebugDays();
int64_t DebugFiles();
int64_t BgDays();
int64_t BgFiles();
int64_t McpLogDays();
int64_t McpLogFiles();
int64_t TerminalRecordDays();

// Core request, MCP, and persistence settings, parsed once after
// ~/.uagent/.config is loaded.
struct RuntimeConfig {
  int64_t first_event_timeout_s = 300;
  int64_t stream_idle_timeout_s = 300;
  int64_t request_timeout_s = 600;
  int64_t request_bytes = 64 * 1024 * 1024;
  int64_t response_bytes = 32 * 1024 * 1024;
  int64_t max_steps = 100;
  int64_t max_tool_calls = 100;
  int64_t max_turn_seconds = 3600;
  double max_turn_cost = 1.0;
  double session_budget = 0;
  int64_t tool_timeout_s = 30;
  int64_t web_search_timeout_s = 25;
  int64_t web_search_max_tokens = 1200;
  int64_t web_search_calls = 4;
  int64_t web_search_max_results = 5;
  int64_t web_search_max_uses = 3;
  int64_t mcp_timeout_s = 60;
  int64_t mcp_servers = 32;
  int64_t mcp_pages = 100;
  int64_t mcp_tools = 256;
  int64_t mcp_config_bytes = 1024 * 1024;
  int64_t mcp_response_bytes = 16 * 1024 * 1024;
  int64_t mcp_schema_bytes = 256 * 1024;
  int64_t mcp_log_bytes = 16 * 1024 * 1024;
  int64_t project_doc_bytes = 32 * 1024;
  int64_t session_archive_bytes = 16 * 1024 * 1024;
  std::string openrouter_provider;
  std::string openrouter_variant;
  std::string web_search_backend = "auto";
  std::string web_search_url;
  std::string web_search_api_key;
  std::string web_search_effort;
  std::string web_search_model;
  std::string web_search_engine = "auto";
  std::string web_search_context_size;
  std::string image_model;
  std::string mcp_roots;
  bool openrouter_fallbacks = true;
  bool web_search_server = true;
  bool memory_enabled = true;

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

  inline static constexpr int64_t kAnyMin = std::numeric_limits<int64_t>::min();
  inline static constexpr int64_t kAnyMax = std::numeric_limits<int64_t>::max();
  inline static constexpr LongOption kLongOptions[] = {
      {"UAGENT_FIRST_EVENT_TIMEOUT", "first_event_timeout_s",
       &RuntimeConfig::first_event_timeout_s, kAnyMin, kAnyMax},
      {"UAGENT_STREAM_IDLE_TIMEOUT", "stream_idle_timeout_s",
       &RuntimeConfig::stream_idle_timeout_s, kAnyMin, kAnyMax},
      {"UAGENT_REQUEST_TIMEOUT", "request_timeout_s",
       &RuntimeConfig::request_timeout_s, kAnyMin, kAnyMax},
      {"UAGENT_REQUEST_BYTES", "request_bytes", &RuntimeConfig::request_bytes,
       1024, kAnyMax},
      {"UAGENT_RESPONSE_BYTES", "response_bytes",
       &RuntimeConfig::response_bytes, kAnyMin, kAnyMax},
      {"UAGENT_MAX_STEPS", "max_steps", &RuntimeConfig::max_steps, 1, kAnyMax},
      {"UAGENT_MAX_TOOL_CALLS", "max_tool_calls",
       &RuntimeConfig::max_tool_calls, 1, kAnyMax},
      {"UAGENT_MAX_TURN_SECONDS", "max_turn_seconds",
       &RuntimeConfig::max_turn_seconds, 1, kAnyMax},
      {"UAGENT_TOOL_TIMEOUT", "tool_timeout_s", &RuntimeConfig::tool_timeout_s,
       0, kAnyMax},
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
  inline static constexpr StringOption kStringOptions[] = {
      {"UAGENT_OPENROUTER_PROVIDER", "openrouter_provider",
       &RuntimeConfig::openrouter_provider, ""},
      {"UAGENT_OPENROUTER_VARIANT", "openrouter_variant",
       &RuntimeConfig::openrouter_variant, ""},
      {"UAGENT_WEB_SEARCH_BACKEND", "web_search_backend",
       &RuntimeConfig::web_search_backend, "auto"},
      {"UAGENT_WEB_SEARCH_URL", "web_search_url",
       &RuntimeConfig::web_search_url, ""},
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
  inline static constexpr BoolOption kBoolOptions[] = {
      {"UAGENT_OPENROUTER_FALLBACKS", "openrouter_fallbacks",
       &RuntimeConfig::openrouter_fallbacks, true},
      {"UAGENT_WEB_SEARCH_SERVER", "web_search_server",
       &RuntimeConfig::web_search_server, true},
      {"UAGENT_MEMORY", "memory_enabled", &RuntimeConfig::memory_enabled, true},
  };

  static RuntimeConfig FromEnvironment();

  json DiagnosticJson() const;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_ENV_H_
