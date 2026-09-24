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

#include "include/core/config_registry.h"
#include "include/core/json.h"

namespace uagent {

// Default model route when nothing is configured: DeepSeek flash through
// OpenRouter auto-routing. One constant so the provider template and side-model
// defaults cannot drift apart.
inline constexpr const char* kDefaultModelRoute =
    "~deepseek/deepseek-flash-latest";

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
int64_t SubagentTimeoutSeconds();
int64_t SubagentCallsPerTurn();
int64_t PersistentMax();
std::string SubagentModel();
std::string TitleModel();
int64_t MaxOutputTokens();
bool SteeringEnabled();
bool SandboxEnabled();
// Outbound TCP. Allowed by default: git, npm and pip all need it.
bool SandboxNetworkAllowed();
std::string SandboxWriteRoots();
bool AdaptiveSystemEnabled();
bool MarkdownEnabled();
bool TrustProjectConfig();
// A delegated child echoes one line per durable event to stderr, which is the
// only way its parent can tell work from a stall before the answer arrives.
bool HeadlessProgressEnabled();
// Experiment overlay for the base prompt: a path, empty when unset.
std::string PromptOverlayPath();

inline bool ValidOpenRouterVariant(std::string_view variant) {
  return Cfg("UAGENT_OPENROUTER_VARIANT").Accepts(variant);
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
enum class ApprovalMode { kAsk, kAuto, kYolo };
const char* ApprovalModeName(ApprovalMode mode);
bool ParseApprovalMode(std::string_view value, ApprovalMode& mode);
ApprovalMode CurrentApprovalMode();
void SetApprovalMode(ApprovalMode mode);
bool ApprovalIsYolo();

// Core request, MCP, and persistence settings. Bootstrap builds one snapshot;
// a validated turn-boundary reload may replace explicitly safe fields.
std::string RuntimeConfigField(std::string_view environment);

// The limits a turn is measured against. A base of RuntimeConfig rather than a
// member of it so that every `config.max_steps` reader keeps working, and so
// that a turn takes its snapshot by slicing -- one assignment that cannot omit
// a budget the way seven hand-written ones could.
// Struct defaults are read from the registry so the two cannot disagree.
template <typename T>
consteval T RegistryDefault(std::string_view environment) {
  return std::get<T>(Cfg(environment).default_value);
}

struct TurnBudgets {
  // Zero disables the model-round limit; turn time, cost, context, process,
  // and tool-call budgets remain independent safety limits.
  int64_t max_steps = RegistryDefault<int64_t>("UAGENT_MAX_STEPS");
  // Zero disables the aggregate per-turn tool-call budget. Individual tools,
  // repeated identical calls, time, and cost remain bounded.
  int64_t max_tool_calls = RegistryDefault<int64_t>("UAGENT_MAX_TOOL_CALLS");
  // Zero disables the aggregate wall-clock turn deadline. Request, stream,
  // tool, repetition, cost, and user-interrupt limits remain independent.
  int64_t max_turn_seconds =
      RegistryDefault<int64_t>("UAGENT_MAX_TURN_SECONDS");
  // Zero disables generated-token limits. Enforcement happens between model
  // rounds, so one response may cross a positive ceiling before the turn stops.
  int64_t max_turn_tokens = RegistryDefault<int64_t>("UAGENT_MAX_TURN_TOKENS");
  int64_t session_token_budget =
      RegistryDefault<int64_t>("UAGENT_SESSION_TOKEN_BUDGET");
  // Zero disables the per-turn reported-cost budget. Users may opt into a
  // positive turn limit or set a separate cumulative session budget.
  double max_turn_cost = RegistryDefault<double>("UAGENT_MAX_TURN_COST");
  double session_budget = RegistryDefault<double>("UAGENT_SESSION_BUDGET");
};

struct RuntimeConfig : TurnBudgets {
  using Values = std::map<std::string, std::string>;
  int64_t first_event_timeout_s =
      RegistryDefault<int64_t>("UAGENT_FIRST_EVENT_TIMEOUT");
  int64_t stream_idle_timeout_s =
      RegistryDefault<int64_t>("UAGENT_STREAM_IDLE_TIMEOUT");
  int64_t request_timeout_s =
      RegistryDefault<int64_t>("UAGENT_REQUEST_TIMEOUT");
  int64_t request_bytes = RegistryDefault<int64_t>("UAGENT_REQUEST_BYTES");
  int64_t response_bytes = RegistryDefault<int64_t>("UAGENT_RESPONSE_BYTES");
  int64_t tool_timeout_s = RegistryDefault<int64_t>("UAGENT_TOOL_TIMEOUT");
  int64_t web_search_timeout_s =
      RegistryDefault<int64_t>("UAGENT_WEB_SEARCH_TIMEOUT");
  int64_t web_search_max_tokens =
      RegistryDefault<int64_t>("UAGENT_WEB_SEARCH_MAX_TOKENS");
  int64_t web_search_calls =
      RegistryDefault<int64_t>("UAGENT_WEB_SEARCH_CALLS");
  int64_t web_search_max_results =
      RegistryDefault<int64_t>("UAGENT_WEB_SEARCH_MAX_RESULTS");
  int64_t web_search_max_uses =
      RegistryDefault<int64_t>("UAGENT_WEB_SEARCH_MAX_USES");
  int64_t mcp_timeout_s = RegistryDefault<int64_t>("UAGENT_MCP_TIMEOUT");
  int64_t mcp_startup_grace_s =
      RegistryDefault<int64_t>("UAGENT_MCP_STARTUP_GRACE");
  int64_t mcp_servers = RegistryDefault<int64_t>("UAGENT_MCP_SERVERS");
  int64_t mcp_pages = RegistryDefault<int64_t>("UAGENT_MCP_PAGES");
  int64_t mcp_tools = RegistryDefault<int64_t>("UAGENT_MCP_TOOLS");
  int64_t mcp_config_bytes =
      RegistryDefault<int64_t>("UAGENT_MCP_CONFIG_BYTES");
  int64_t mcp_response_bytes =
      RegistryDefault<int64_t>("UAGENT_MCP_RESPONSE_BYTES");
  int64_t mcp_schema_bytes =
      RegistryDefault<int64_t>("UAGENT_MCP_SCHEMA_BYTES");
  int64_t mcp_log_bytes = RegistryDefault<int64_t>("UAGENT_MCP_LOG_BYTES");
  int64_t memory_always_bytes =
      RegistryDefault<int64_t>("UAGENT_MEMORY_ALWAYS_BYTES");
  int64_t project_doc_bytes =
      RegistryDefault<int64_t>("UAGENT_PROJECT_DOC_BYTES");
  int64_t session_archive_bytes =
      RegistryDefault<int64_t>("UAGENT_SESSION_ARCHIVE_BYTES");
  std::string approval{RegistryDefault<std::string_view>("UAGENT_APPROVAL")};
  std::string permission_model{
      RegistryDefault<std::string_view>("UAGENT_PERMISSION_MODEL")};
  std::string permission_url{
      RegistryDefault<std::string_view>("UAGENT_PERMISSION_URL")};
  std::string openrouter_provider{
      RegistryDefault<std::string_view>("UAGENT_OPENROUTER_PROVIDER")};
  std::string openrouter_variant{
      RegistryDefault<std::string_view>("UAGENT_OPENROUTER_VARIANT")};
  std::string web_search_backend{
      RegistryDefault<std::string_view>("UAGENT_WEB_SEARCH_BACKEND")};
  std::string web_search_effort{
      RegistryDefault<std::string_view>("UAGENT_WEB_SEARCH_EFFORT")};
  std::string web_search_model{
      RegistryDefault<std::string_view>("UAGENT_WEB_SEARCH_MODEL")};
  std::string web_search_engine{
      RegistryDefault<std::string_view>("UAGENT_WEB_SEARCH_ENGINE")};
  std::string web_search_context_size{
      RegistryDefault<std::string_view>("UAGENT_WEB_SEARCH_CONTEXT_SIZE")};
  std::string image_model{
      RegistryDefault<std::string_view>("UAGENT_IMAGE_MODEL")};
  // OpenRouter file-parser engine for documents a model cannot read natively:
  // cloudflare-ai (free), mistral-ocr (scans, billed per page) or native.
  std::string pdf_engine{
      RegistryDefault<std::string_view>("UAGENT_PDF_ENGINE")};
  std::string mcp_roots{RegistryDefault<std::string_view>("UAGENT_MCP_ROOTS")};
  bool openrouter_fallbacks =
      RegistryDefault<bool>("UAGENT_OPENROUTER_FALLBACKS");
  bool memory_enabled = RegistryDefault<bool>("UAGENT_MEMORY");
  bool memory_generate = RegistryDefault<bool>("UAGENT_MEMORY_GENERATE");

  static RuntimeConfig FromEnvironment();
  static RuntimeConfig FromValues(const Values& values);
  std::vector<std::string> ApplyTurnReload(const RuntimeConfig& next);

  json DiagnosticJson() const;
  json ProvenanceJson(const json& env_sources) const;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_ENV_H_
