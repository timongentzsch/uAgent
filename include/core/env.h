// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_ENV_H_
#define UAGENT_INCLUDE_CORE_ENV_H_
// Environment lookups and the parsed runtime configuration. Session-static
// fields live on RuntimeConfig; the env-to-field tables live in env.cc.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "include/core/json.h"

namespace uagent {

std::string EnvStr(const char* name, const std::string& dflt = "");

int64_t EnvLong(const char* name, int64_t dflt);

double EnvDouble(const char* name, double dflt);

// Strict: only the ParseBool spellings count, so a misspelled switch reads as
// the default rather than as "set, therefore on".
bool EnvBool(const char* name, bool dflt);

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
int64_t SubagentTimeoutSeconds();
int64_t SubagentCallsPerTurn();
std::string SubagentModel();
int64_t MaxOutputTokens();
bool SteeringEnabled();
bool AdaptiveSystemEnabled();
// Experiment overlay for the base prompt: a path, empty when unset.
std::string PromptOverlayPath();

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
int64_t SkillBodyBytes();
// Descriptions stay bounded because discovery may return several at once;
// bodies are sent only when a skill is opened.
int64_t SkillDescriptionBytes();
int64_t MaxSkills();
// The download cap for one web_fetch. Pages beyond it are read as far as the
// cap and marked partial rather than failed.
int64_t WebFetchBytes();
int64_t GrepResults();
int64_t GrepBytes();
int64_t BashLogBytes();
int64_t RunDefaultYieldMs();
int64_t MaxBackgroundJobs();
int64_t McpConfigBytes();
int64_t McpDescriptionChars();
int64_t MaxPendingAttachments();
int64_t AttachmentLimitMb();
// One default, shared by the attachment path and MCP image results.
int64_t TerminalImageLimitMb();
int64_t ImageMaxColumns();
std::string ImageProtocol();
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
std::string ShellEnvironmentAllowlist();

// Approval mode is the one setting a running session can toggle, so it cannot
// live in environ: spawning a child iterates environ on another thread while
// /yolo would be rewriting it. Children receive it as an explicit override.
bool ApprovalIsAutomatic();
void SetApprovalAutomatic(bool automatic);

// Core request, MCP, and persistence settings. Bootstrap builds one snapshot;
// a validated turn-boundary reload may replace explicitly safe fields.
std::string RuntimeConfigField(std::string_view environment);

// The limits a turn is measured against. A base of RuntimeConfig rather than a
// member of it so that every `config.max_steps` reader keeps working, and so
// that a turn takes its snapshot by slicing -- one assignment that cannot omit
// a budget the way seven hand-written ones could.
struct TurnBudgets {
  // Zero disables the model-round limit; turn time, cost, context, process,
  // and tool-call budgets remain independent safety limits.
  int64_t max_steps = 0;
  // Zero disables the aggregate per-turn tool-call budget. Individual tools,
  // repeated identical calls, time, and cost remain bounded.
  int64_t max_tool_calls = 0;
  // Zero disables the aggregate wall-clock turn deadline. Request, stream,
  // tool, repetition, cost, and user-interrupt limits remain independent.
  int64_t max_turn_seconds = 0;
  // Zero disables generated-token limits. Enforcement happens between model
  // rounds, so one response may cross a positive ceiling before the turn stops.
  int64_t max_turn_tokens = 0;
  int64_t session_token_budget = 0;
  // Zero disables the per-turn reported-cost budget. Users may opt into a
  // positive turn limit or set a separate cumulative session budget.
  double max_turn_cost = 0;
  double session_budget = 0;
};

struct RuntimeConfig : TurnBudgets {
  using Values = std::map<std::string, std::string>;
  int64_t first_event_timeout_s = 300;
  int64_t stream_idle_timeout_s = 300;
  int64_t request_timeout_s = 600;
  int64_t request_bytes = int64_t{64} * 1024 * 1024;
  int64_t response_bytes = int64_t{32} * 1024 * 1024;
  int64_t tool_timeout_s = 30;
  int64_t web_search_timeout_s = 60;
  int64_t web_search_max_tokens = 1200;
  int64_t web_search_calls = 4;
  int64_t web_search_max_results = 5;
  int64_t web_search_max_uses = 3;
  int64_t mcp_timeout_s = 60;
  int64_t mcp_startup_grace_s = 2;
  int64_t mcp_servers = 32;
  int64_t mcp_pages = 100;
  int64_t mcp_tools = 256;
  int64_t mcp_config_bytes = int64_t{1024} * 1024;
  int64_t mcp_response_bytes = int64_t{16} * 1024 * 1024;
  int64_t mcp_schema_bytes = int64_t{256} * 1024;
  int64_t mcp_log_bytes = int64_t{16} * 1024 * 1024;
  int64_t memory_always_bytes = 2048;
  int64_t project_doc_bytes = int64_t{32} * 1024;
  int64_t session_archive_bytes = int64_t{16} * 1024 * 1024;
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
  // OpenRouter file-parser engine for documents a model cannot read natively:
  // cloudflare-ai (free), mistral-ocr (scans, billed per page) or native.
  std::string pdf_engine;
  std::string mcp_roots;
  bool openrouter_fallbacks = true;
  bool memory_enabled = true;
  bool memory_generate = true;

  static RuntimeConfig FromEnvironment();
  static RuntimeConfig FromValues(const Values& values);
  std::vector<std::string> ApplyTurnReload(const RuntimeConfig& next);

  json DiagnosticJson() const;
  json ProvenanceJson(const json& env_sources) const;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_ENV_H_
