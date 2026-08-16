// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_ENV_H_
#define UAGENT_INCLUDE_CORE_ENV_H_
// Environment lookups and the parsed runtime configuration. Session-static
// fields live on RuntimeConfig; the env-to-field tables live in env.cc.

#include <cstdint>
#include <string>
#include <string_view>

#include "include/core/json.h"

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
int64_t AutoCompactTokens();
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
bool AdaptiveSystemEnabled();

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
int64_t MemoryIdleSeconds();
int64_t MemoryExtractBytes();
int64_t MemoryAlwaysBytes();
int64_t SkillBodyBytes();
// Descriptions stay bounded because discovery may return several at once;
// bodies are sent only when a skill is opened.
int64_t SkillDescriptionBytes();
int64_t MaxSkills();
int64_t GrepResults();
int64_t GrepBytes();
int64_t BashLogBytes();
int64_t RunDefaultYieldMs();
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
  int64_t max_steps = 0;
  // Zero disables the model-round limit; turn time, cost, context, process,
  // and tool-call budgets remain independent safety limits.
  // Zero disables the aggregate per-turn tool-call budget. Individual tools,
  // repeated identical calls, time, and cost remain bounded.
  int64_t max_tool_calls = 0;
  int64_t max_turn_seconds = 3600;
  // Zero disables the per-turn reported-cost budget. Users may opt into a
  // positive turn limit or set a separate cumulative session budget.
  double max_turn_cost = 0;
  double session_budget = 0;
  int64_t tool_timeout_s = 30;
  int64_t web_search_timeout_s = 60;
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
  bool memory_enabled = true;
  bool memory_generate = true;

  static RuntimeConfig FromEnvironment();

  json DiagnosticJson() const;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_ENV_H_
