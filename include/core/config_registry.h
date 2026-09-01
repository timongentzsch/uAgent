// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_CONFIG_REGISTRY_H_
#define UAGENT_INCLUDE_CORE_CONFIG_REGISTRY_H_
// The authoritative description of every UAGENT_* setting: default, bounds,
// when a change takes effect, and whether the value may be shown. Runtime
// getters, RuntimeConfig, the self-description tool and the generated skill
// references all read these descriptors, so a documented default cannot drift
// from the one the binary applies.
//
// `environment` and `field` hold string literals, so `.data()` is
// null-terminated and safe to hand to getenv.

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "include/core/limits.h"
#include "include/core/signals.h"

namespace uagent {

inline constexpr int64_t kConfigAnyMin = std::numeric_limits<int64_t>::min();
inline constexpr int64_t kConfigAnyMax = std::numeric_limits<int64_t>::max();
inline constexpr int64_t kConfigMaxMinusOne = kConfigAnyMax - 1;
inline constexpr int64_t kConfigMaxMegabytes = static_cast<int64_t>(
    std::numeric_limits<size_t>::max() / (int64_t{1024} * 1024));

enum class ConfigType { kInt, kDouble, kBool, kString };

// Where a committed change becomes visible. Config files are exported into the
// process environment once at startup, and only RuntimeConfig is re-read at a
// user-turn boundary, so a setting reached through a live getenv getter needs a
// restart even though nothing freezes its value.
enum class ReloadPolicy { kNextUserTurn, kRestartRequired };

enum class Sensitivity { kPublic, kSecret, kCompositeSecret };

// Bit flags: a setting may be persistable at more than one layer.
enum ConfigScope : unsigned {
  kScopeUser = 1u << 0,
  kScopeProject = 1u << 1,
};

using ConfigDefault =
    std::variant<std::monostate, int64_t, double, bool, std::string_view>;

struct ConfigDescriptor {
  const char* EnvName() const {
    return environment
        .data();  // NOLINT(bugprone-suspicious-stringview-data-usage)
  }

  std::string_view environment;
  std::string_view field;  // RuntimeConfig member; empty for direct getters
  ConfigType type = ConfigType::kInt;
  ConfigDefault default_value;
  int64_t minimum = kConfigAnyMin;
  int64_t maximum = kConfigAnyMax;
  ReloadPolicy reload = ReloadPolicy::kRestartRequired;
  Sensitivity sensitivity = Sensitivity::kPublic;
  unsigned scopes = kScopeUser | kScopeProject;
  std::string_view category;
  std::string_view description;
};

namespace registry {

inline constexpr int64_t kMb = int64_t{1024} * 1024;

// Shorthand keeps one row on one line so the table stays readable; every field
// after the bounds is spelled out at the call site.
consteval ConfigDescriptor Int(std::string_view env, std::string_view field,
                               int64_t value, int64_t minimum, int64_t maximum,
                               ReloadPolicy reload, std::string_view category,
                               std::string_view description) {
  return {env,
          field,
          ConfigType::kInt,
          value,
          minimum,
          maximum,
          reload,
          Sensitivity::kPublic,
          kScopeUser | kScopeProject,
          category,
          description};
}

consteval ConfigDescriptor Str(std::string_view env, std::string_view field,
                               std::string_view value, ReloadPolicy reload,
                               Sensitivity sensitivity,
                               std::string_view category,
                               std::string_view description) {
  return {env,      field,         ConfigType::kString,
          value,    kConfigAnyMin, kConfigAnyMax,
          reload,   sensitivity,   kScopeUser | kScopeProject,
          category, description};
}

consteval ConfigDescriptor Bul(std::string_view env, std::string_view field,
                               bool value, ReloadPolicy reload,
                               std::string_view category,
                               std::string_view description) {
  return {env,
          field,
          ConfigType::kBool,
          value,
          kConfigAnyMin,
          kConfigAnyMax,
          reload,
          Sensitivity::kPublic,
          kScopeUser | kScopeProject,
          category,
          description};
}

consteval ConfigDescriptor Dbl(std::string_view env, std::string_view field,
                               double value, ReloadPolicy reload,
                               std::string_view category,
                               std::string_view description) {
  return {env,
          field,
          ConfigType::kDouble,
          value,
          kConfigAnyMin,
          kConfigAnyMax,
          reload,
          Sensitivity::kPublic,
          kScopeUser | kScopeProject,
          category,
          description};
}

// A default computed from another setting rather than a constant.
consteval ConfigDescriptor Derived(std::string_view env, int64_t minimum,
                                   int64_t maximum, std::string_view category,
                                   std::string_view description) {
  return {env,
          {},
          ConfigType::kInt,
          std::monostate{},
          minimum,
          maximum,
          ReloadPolicy::kRestartRequired,
          Sensitivity::kPublic,
          kScopeUser | kScopeProject,
          category,
          description};
}

}  // namespace registry

inline constexpr ConfigDescriptor kConfigRegistry[] = {
    // Route selection and credentials.
    registry::Str("UAGENT_BASE_URL", {}, "", ReloadPolicy::kRestartRequired,
                  Sensitivity::kPublic, "route", "active API base URL"),
    registry::Str("UAGENT_API_KEY", {}, "sk-noop",
                  ReloadPolicy::kRestartRequired, Sensitivity::kSecret, "route",
                  "credential for the active route"),
    registry::Str("OPENROUTER_API_KEY", {}, "", ReloadPolicy::kRestartRequired,
                  Sensitivity::kSecret, "route",
                  "OpenRouter credential used when no base URL is set"),
    registry::Str(
        "UAGENT_MODEL", {}, "", ReloadPolicy::kRestartRequired,
        Sensitivity::kPublic, "route",
        "model or named route as [provider/]model[:variant][:effort]"),
    registry::Str("UAGENT_REASONING_EFFORT", {}, "",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic, "route",
                  "none, minimal, low, medium, high, xhigh, or max"),
    registry::Str("UAGENT_PROVIDERS", {}, "", ReloadPolicy::kRestartRequired,
                  Sensitivity::kCompositeSecret, "route",
                  "JSON object of named endpoints, transports, and aliases"),
    registry::Str("UAGENT_WIRE_API", {}, "chat_completions",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic, "route",
                  "chat_completions, responses, or anthropic_messages"),
    registry::Str("UAGENT_HOSTED_TOOLS", {}, "", ReloadPolicy::kRestartRequired,
                  Sensitivity::kPublic, "route",
                  "comma-separated hosted capabilities; currently web_search"),
    registry::Str("UAGENT_PROVIDER_PROTOCOL", {}, "",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic, "route",
                  "openai, openrouter, or anthropic"),
    registry::Str("UAGENT_OPENROUTER_COMPATIBLE", {}, "",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic, "route",
                  "treat a custom base URL as OpenRouter-compatible"),
    registry::Str("UAGENT_OPENROUTER_PROVIDER", "openrouter_provider", "",
                  ReloadPolicy::kNextUserTurn, Sensitivity::kPublic, "route",
                  "pin OpenRouter to one upstream provider"),
    registry::Str("UAGENT_OPENROUTER_VARIANT", "openrouter_variant", "",
                  ReloadPolicy::kNextUserTurn, Sensitivity::kPublic, "route",
                  "nitro, floor, or exacto routing preference"),
    registry::Bul("UAGENT_OPENROUTER_FALLBACKS", "openrouter_fallbacks", true,
                  ReloadPolicy::kNextUserTurn, "route",
                  "allow OpenRouter to fall back to another provider"),
    registry::Int("UAGENT_CONTEXT", {}, 0, kConfigAnyMin, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "route",
                  "context-window tokens; 0 uses the provider profile"),
    registry::Int("UAGENT_MAX_TOKENS", {}, -1, kConfigAnyMin, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "route",
                  "maximum response tokens; -1 omits the optional cap"),

    // Request transport.
    registry::Int("UAGENT_FIRST_EVENT_TIMEOUT", "first_event_timeout_s", 300,
                  kConfigAnyMin, kConfigAnyMax, ReloadPolicy::kNextUserTurn,
                  "request", "seconds to wait for the first streamed event"),
    registry::Int("UAGENT_STREAM_IDLE_TIMEOUT", "stream_idle_timeout_s", 300,
                  kConfigAnyMin, kConfigAnyMax, ReloadPolicy::kNextUserTurn,
                  "request", "seconds of stream silence before aborting"),
    registry::Int("UAGENT_REQUEST_TIMEOUT", "request_timeout_s", 600,
                  kConfigAnyMin, kConfigAnyMax, ReloadPolicy::kNextUserTurn,
                  "request", "total seconds allowed for one model request"),
    registry::Int("UAGENT_REQUEST_BYTES", "request_bytes", 64 * registry::kMb,
                  1024, kConfigAnyMax, ReloadPolicy::kNextUserTurn, "request",
                  "maximum serialized request size"),
    registry::Int("UAGENT_RESPONSE_BYTES", "response_bytes", 32 * registry::kMb,
                  kConfigAnyMin, kConfigAnyMax, ReloadPolicy::kNextUserTurn,
                  "request", "maximum accumulated response size"),

    // Turn budgets.
    registry::Int("UAGENT_MAX_STEPS", "max_steps", 0, 0, kConfigAnyMax,
                  ReloadPolicy::kNextUserTurn, "budget",
                  "model rounds per turn; 0 disables the limit"),
    registry::Int("UAGENT_MAX_TOOL_CALLS", "max_tool_calls", 0, 0,
                  kConfigAnyMax, ReloadPolicy::kNextUserTurn, "budget",
                  "tool calls per turn; 0 disables the limit"),
    registry::Int("UAGENT_MAX_TURN_SECONDS", "max_turn_seconds", 0, 0,
                  kConfigAnyMax, ReloadPolicy::kNextUserTurn, "budget",
                  "wall-clock seconds per turn; 0 disables the deadline"),
    registry::Int("UAGENT_MAX_TURN_TOKENS", "max_turn_tokens", 0, 0,
                  kConfigAnyMax, ReloadPolicy::kNextUserTurn, "budget",
                  "generated-token ceiling per turn; 0 disables it"),
    registry::Int("UAGENT_SESSION_TOKEN_BUDGET", "session_token_budget", 0,
                  0, kConfigAnyMax, ReloadPolicy::kNextUserTurn, "budget",
                  "cumulative generated-token ceiling; 0 disables it"),
    registry::Dbl("UAGENT_MAX_TURN_COST", "max_turn_cost", 0.0,
                  ReloadPolicy::kNextUserTurn, "budget",
                  "reported-cost ceiling per turn; 0 disables it"),
    registry::Dbl("UAGENT_SESSION_BUDGET", "session_budget", 0.0,
                  ReloadPolicy::kNextUserTurn, "budget",
                  "cumulative reported-cost ceiling; 0 disables it"),
    registry::Int("UAGENT_TOOL_TIMEOUT", "tool_timeout_s", 30, 0, kConfigAnyMax,
                  ReloadPolicy::kNextUserTurn, "budget",
                  "seconds one tool call may run"),
    registry::Int("UAGENT_TOOL_CONCURRENCY", {}, 4, 1, kFgMax,
                  ReloadPolicy::kRestartRequired, "budget",
                  "parallel foreground tool workers"),
    registry::Int("UAGENT_AUTO_COMPACT_PCT", {}, 85, kConfigAnyMin,
                  kConfigAnyMax, ReloadPolicy::kRestartRequired, "budget",
                  "context percentage that triggers compaction"),
    registry::Int(
        "UAGENT_AUTO_COMPACT_TOKENS", {}, 0, 0, kConfigAnyMax,
        ReloadPolicy::kRestartRequired, "budget",
        "absolute token trigger for compaction; 0 uses the percentage"),

    // Tool results and trace retention.
    registry::Int("UAGENT_TOOL_RESULT_CHARS", {}, 8000, kConfigAnyMin,
                  kConfigAnyMax, ReloadPolicy::kRestartRequired, "tools",
                  "characters kept from one tool result"),
    registry::Int("UAGENT_TOOL_TRACE_PROTECT_CHARS", {}, int64_t{64} * 1024, 0,
                  kConfigAnyMax, ReloadPolicy::kRestartRequired, "tools",
                  "recent tool output never pruned from the trace"),
    registry::Int("UAGENT_TOOL_TRACE_PRUNE_MIN_CHARS", {}, int64_t{32} * 1024,
                  0, kConfigAnyMax, ReloadPolicy::kRestartRequired, "tools",
                  "smallest tool result the trace pruner will drop"),
    registry::Int("UAGENT_READ_FILE_LINES", {}, 1000, kConfigAnyMin,
                  kConfigAnyMax, ReloadPolicy::kRestartRequired, "tools",
                  "default lines returned by read_path"),
    registry::Int("UAGENT_READ_FILE_MAX_LINES", {}, 10000, 1, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "tools",
                  "maximum lines one read_path call may request"),
    registry::Int("UAGENT_READ_FILE_BYTES", {}, int64_t{32} * 1024, 1024,
                  kConfigAnyMax, ReloadPolicy::kRestartRequired, "tools",
                  "maximum bytes returned by read_path"),
    registry::Int("UAGENT_EDIT_FILE_BYTES", {}, 10 * registry::kMb,
                  kConfigAnyMin, kConfigAnyMax, ReloadPolicy::kRestartRequired,
                  "tools", "largest file edit_file will rewrite"),
    registry::Int("UAGENT_LIST_DIR_ENTRIES", {}, 1000, 1, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "tools",
                  "directory entries returned"),
    registry::Int("UAGENT_LIST_DIR_SCAN_ENTRIES", {}, 100000, 1, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "tools",
                  "directory entries scanned before giving up"),
    registry::Int("UAGENT_GREP_RESULTS", {}, 200, 1, kConfigMaxMinusOne,
                  ReloadPolicy::kRestartRequired, "tools", "grep matches kept"),
    registry::Derived("UAGENT_GREP_BYTES", 1024, kConfigAnyMax, "tools",
                      "grep result bytes; defaults to the tool-result cap"),
    registry::Int("UAGENT_BASH_LOG_BYTES", {}, 64 * registry::kMb, 1024,
                  kConfigAnyMax, ReloadPolicy::kRestartRequired, "tools",
                  "bounded rotating process log"),
    registry::Int("UAGENT_RUN_YIELD_MS", {}, 10000, 0, kMaxYieldMs,
                  ReloadPolicy::kRestartRequired, "tools",
                  "default initial wait for run; 0 disables yielding"),
    registry::Int("UAGENT_MAX_BACKGROUND_JOBS", {}, 8, 1, kBgMax,
                  ReloadPolicy::kRestartRequired, "tools",
                  "concurrent detached activities"),
    registry::Derived("UAGENT_WEB_FETCH_BYTES", 1024, kConfigAnyMax, "tools",
                      "web_fetch download cap; defaults to the attachment cap"),

    // Delegation.
    registry::Int("UAGENT_DEPTH", {}, 0, 0, kConfigMaxMinusOne,
                  ReloadPolicy::kRestartRequired, "delegation",
                  "delegation depth of this process; 0 is the coordinator"),
    registry::Int("UAGENT_SUBAGENT_DEPTH", {}, 2, 0, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "delegation",
                  "deepest delegation level allowed"),
    registry::Int("UAGENT_SUBAGENT_MAX_STEPS", {}, 100, kConfigAnyMin,
                  kConfigAnyMax, ReloadPolicy::kRestartRequired, "delegation",
                  "model rounds per delegated child"),
    registry::Int("UAGENT_SUBAGENT_MAX_TOOL_CALLS", {}, 240, kConfigAnyMin,
                  kConfigAnyMax, ReloadPolicy::kRestartRequired, "delegation",
                  "tool calls per delegated child"),
    registry::Int("UAGENT_SUBAGENT_TIMEOUT", {}, 0, 0, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "delegation",
                  "wall-clock ceiling per delegated child; 0 is the turn"),
    registry::Int("UAGENT_SUBAGENT_CALLS_PER_TURN", {}, 32, 1, 500,
                  ReloadPolicy::kRestartRequired, "delegation",
                  "delegated children one coordinator turn may start"),
    registry::Str("UAGENT_SUBAGENT_MODEL", {}, "",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                  "delegation", "default model route for delegated children"),
    registry::Str("UAGENT_TOOLSET", {}, "", ReloadPolicy::kRestartRequired,
                  Sensitivity::kPublic, "delegation",
                  "lean withholds implementation tools from this process"),

    // Web search.
    registry::Str("UAGENT_WEB_SEARCH_BACKEND", "web_search_backend", "auto",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                  "search", "auto, openrouter, or off"),
    registry::Str("UAGENT_WEB_SEARCH_URL", "web_search_url", "",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                  "search", "OpenRouter-compatible search endpoint"),
    registry::Str("UAGENT_WEB_SEARCH_API_KEY", "web_search_api_key", "",
                  ReloadPolicy::kRestartRequired, Sensitivity::kSecret,
                  "search", "credential for the separate search route"),
    registry::Str("UAGENT_WEB_SEARCH_MODEL", "web_search_model", "",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                  "search", "model route used for search"),
    registry::Str("UAGENT_WEB_SEARCH_EFFORT", "web_search_effort", "",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                  "search", "reasoning effort for the search route"),
    registry::Str("UAGENT_WEB_SEARCH_ENGINE", "web_search_engine", "auto",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                  "search",
                  "auto, native, exa, firecrawl, parallel, perplexity"),
    registry::Str("UAGENT_WEB_SEARCH_CONTEXT_SIZE", "web_search_context_size",
                  "", ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                  "search", "low, medium, or high native search context"),
    registry::Int("UAGENT_WEB_SEARCH_TIMEOUT", "web_search_timeout_s", 60, 1,
                  kConfigAnyMax, ReloadPolicy::kNextUserTurn, "search",
                  "seconds allowed for one search request"),
    registry::Int("UAGENT_WEB_SEARCH_MAX_TOKENS", "web_search_max_tokens", 1200,
                  128, kConfigAnyMax, ReloadPolicy::kNextUserTurn, "search",
                  "tokens returned by one search"),
    registry::Int("UAGENT_WEB_SEARCH_CALLS", "web_search_calls", 4, 1,
                  kConfigAnyMax, ReloadPolicy::kNextUserTurn, "search",
                  "search calls allowed per turn"),
    registry::Int("UAGENT_WEB_SEARCH_MAX_RESULTS", "web_search_max_results", 5,
                  1, 25, ReloadPolicy::kNextUserTurn, "search",
                  "results requested per search"),
    registry::Int("UAGENT_WEB_SEARCH_MAX_USES", "web_search_max_uses", 3, 1, 30,
                  ReloadPolicy::kNextUserTurn, "search",
                  "hosted search invocations per request"),

    // Memory.
    registry::Bul("UAGENT_MEMORY", "memory_enabled", true,
                  ReloadPolicy::kRestartRequired, "memory",
                  "enable memory recall and writes"),
    registry::Bul("UAGENT_MEMORY_GENERATE", "memory_generate", true,
                  ReloadPolicy::kRestartRequired, "memory",
                  "run the background memory extractor"),
    registry::Int("UAGENT_MEMORY_ALWAYS_BYTES", "memory_always_bytes", 2048, 0,
                  int64_t{64} * 1024, ReloadPolicy::kRestartRequired, "memory",
                  "always-on memory slice injected into the prompt"),
    registry::Int("UAGENT_MEMORY_BYTES", {}, 2048, 256, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "memory",
                  "bytes stored per memory"),
    registry::Int("UAGENT_MEMORY_FILES", {}, 32, 1, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "memory",
                  "memories retained"),
    registry::Int("UAGENT_MEMORY_IDLE_SECONDS", {}, int64_t{6} * 60 * 60, 0,
                  int64_t{48} * 60 * 60, ReloadPolicy::kRestartRequired,
                  "memory", "idle seconds before background extraction runs"),
    registry::Int("UAGENT_MEMORY_EXTRACT_BYTES", {}, int64_t{32} * 1024, 4096,
                  int64_t{256} * 1024, ReloadPolicy::kRestartRequired, "memory",
                  "transcript bytes handed to the extractor"),
    registry::Str("UAGENT_MEMORY_MODEL", {}, "", ReloadPolicy::kRestartRequired,
                  Sensitivity::kPublic, "memory",
                  "model route for background memory extraction"),

    // Skills.
    registry::Int("UAGENT_SKILL_BYTES", {}, int64_t{512} * 1024, 1024,
                  registry::kMb, ReloadPolicy::kRestartRequired, "skills",
                  "largest skill body loaded when opened"),
    registry::Int("UAGENT_SKILL_DESC_BYTES", {}, 1024, 16, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "skills",
                  "skill description bytes shown in the catalogue"),
    registry::Int("UAGENT_SKILLS", {}, 64, 1, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "skills",
                  "skills discovered"),
    registry::Str("UAGENT_SKILL_PATH", {}, "", ReloadPolicy::kRestartRequired,
                  Sensitivity::kPublic, "skills",
                  "replace the entire skill search path"),
    registry::Str("UAGENT_SKILL_EXCLUDE", {}, "",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                  "skills", "comma-separated skill names to withhold"),

    // MCP.
    registry::Int("UAGENT_MCP_TIMEOUT", "mcp_timeout_s", 60, 1, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "mcp",
                  "seconds allowed for one MCP call"),
    registry::Int("UAGENT_MCP_STARTUP_GRACE", "mcp_startup_grace_s", 2, 0,
                  kConfigAnyMax, ReloadPolicy::kRestartRequired, "mcp",
                  "shared startup seconds for optional MCP servers"),
    registry::Int("UAGENT_MCP_SERVERS", "mcp_servers", 32, 1, kMcpMax,
                  ReloadPolicy::kRestartRequired, "mcp",
                  "MCP servers registered"),
    registry::Int("UAGENT_MCP_PAGES", "mcp_pages", 100, 1, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "mcp",
                  "pages walked while listing MCP tools"),
    registry::Int("UAGENT_MCP_TOOLS", "mcp_tools", 256, 1, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "mcp",
                  "MCP tools registered"),
    registry::Int("UAGENT_MCP_CONFIG_BYTES", "mcp_config_bytes", registry::kMb,
                  1024, kConfigAnyMax, ReloadPolicy::kRestartRequired, "mcp",
                  "largest .mcp.json accepted"),
    registry::Int("UAGENT_MCP_RESPONSE_BYTES", "mcp_response_bytes",
                  16 * registry::kMb, 1024, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "mcp",
                  "largest MCP response accepted"),
    registry::Int("UAGENT_MCP_SCHEMA_BYTES", "mcp_schema_bytes",
                  int64_t{256} * 1024, 1024, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "mcp",
                  "largest MCP tool schema accepted"),
    registry::Int("UAGENT_MCP_LOG_BYTES", "mcp_log_bytes", 16 * registry::kMb,
                  1024, kConfigAnyMax, ReloadPolicy::kRestartRequired, "mcp",
                  "bounded MCP server log"),
    registry::Int("UAGENT_MCP_DESC_CHARS", {}, 400, kConfigAnyMin,
                  kConfigAnyMax, ReloadPolicy::kRestartRequired, "mcp",
                  "MCP tool description characters kept"),
    registry::Str("UAGENT_MCP_ROOTS", "mcp_roots", "",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic, "mcp",
                  "roots advertised to MCP servers"),

    // Attachments and terminal media.
    registry::Str("UAGENT_IMAGE_MODEL", "image_model", "",
                  ReloadPolicy::kNextUserTurn, Sensitivity::kPublic, "media",
                  "model route that reads attached images"),
    registry::Str("UAGENT_IMAGE_DETAIL", {}, "", ReloadPolicy::kRestartRequired,
                  Sensitivity::kPublic, "media",
                  "low, high, or original image detail"),
    registry::Str("UAGENT_IMAGE_PROTOCOL", {}, "",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic, "media",
                  "terminal image protocol: auto, iterm, kitty, or none"),
    registry::Str("UAGENT_PDF_ENGINE", "pdf_engine", "cloudflare-ai",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic, "media",
                  "OpenRouter file-parser engine for documents"),
    registry::Int("UAGENT_PENDING_ATTACHMENTS", {}, 8, 1, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "media",
                  "attachments queued for the next turn"),
    registry::Int("UAGENT_ATTACHMENT_MB", {}, 10, 1, kConfigMaxMegabytes,
                  ReloadPolicy::kRestartRequired, "media",
                  "largest attachment in mebibytes"),
    registry::Int("UAGENT_TERMINAL_IMAGE_MB", {}, 10, 1, kConfigMaxMegabytes,
                  ReloadPolicy::kRestartRequired, "media",
                  "largest terminal-rendered image in mebibytes"),
    registry::Int("UAGENT_IMAGE_MAX_COLUMNS", {}, 200, 1, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "media",
                  "widest terminal image in columns"),
    registry::Derived("UAGENT_IMAGE_COLUMNS", kConfigAnyMin, kConfigAnyMax,
                      "media",
                      "terminal image width; defaults to the available width"),

    // Session and artifact retention.
    registry::Int("UAGENT_PROJECT_DOC_BYTES", "project_doc_bytes",
                  int64_t{32} * 1024, 0, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "retention",
                  "AGENTS.md bytes injected into the prompt"),
    registry::Int("UAGENT_SESSION_ARCHIVE_BYTES", "session_archive_bytes",
                  16 * registry::kMb, 0, kConfigAnyMax,
                  ReloadPolicy::kNextUserTurn, "retention",
                  "compacted transcript bytes retained"),
    registry::Int("UAGENT_HISTORY_DAYS", {}, 30, kConfigAnyMin, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "retention",
                  "days of saved sessions kept"),
    registry::Int("UAGENT_HISTORY_FILES", {}, 200, kConfigAnyMin, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "retention",
                  "saved sessions kept"),
    registry::Int("UAGENT_DEBUG_DAYS", {}, 14, kConfigAnyMin, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "retention",
                  "days of debug traces kept"),
    registry::Int("UAGENT_DEBUG_FILES", {}, 50, kConfigAnyMin, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "retention",
                  "debug traces kept"),
    registry::Int("UAGENT_BG_DAYS", {}, 7, kConfigAnyMin, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "retention",
                  "days of background logs kept"),
    registry::Int("UAGENT_BG_FILES", {}, 200, kConfigAnyMin, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "retention",
                  "background logs kept"),
    registry::Int("UAGENT_MCP_LOG_DAYS", {}, 7, kConfigAnyMin, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "retention",
                  "days of MCP logs kept"),
    registry::Int("UAGENT_MCP_LOG_FILES", {}, 100, kConfigAnyMin, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "retention", "MCP logs kept"),
    registry::Int("UAGENT_TERMINAL_DAYS", {}, 7, 0, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "retention",
                  "days of terminal recordings kept"),

    // Behaviour switches.
    registry::Bul("UAGENT_STEERING", {}, true, ReloadPolicy::kRestartRequired,
                  "behaviour", "accept typed steering during a turn"),
    registry::Bul("UAGENT_ADAPT_SYSTEM", {}, false,
                  ReloadPolicy::kRestartRequired, "behaviour",
                  "expose adapt_system so the model may revise its directive"),
    registry::Str("UAGENT_PROMPT_OVERLAY", {}, "",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                  "behaviour",
                  "experiment: JSON file replacing base prompt sections so a "
                  "variant can be measured without a rebuild; prompt text "
                  "only"),
    registry::Str("UAGENT_APPROVAL", {}, "", ReloadPolicy::kRestartRequired,
                  Sensitivity::kPublic, "behaviour",
                  "yolo approves ordinary mutations without asking"),
    registry::Str("UAGENT_TOOL_CAPABILITIES", {}, "",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                  "behaviour", "restrict the exposed tool capability set"),
    registry::Str(
        "UAGENT_SHELL_ENV_ALLOW", {}, "", ReloadPolicy::kRestartRequired,
        Sensitivity::kPublic, "behaviour",
        "comma-separated sensitive variables approved shells may inherit"),
    registry::Str("UAGENT_TRUST_PROJECT_CONFIG", {}, "",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                  "behaviour", "trust this workspace's .mcp.json and config"),
    registry::Str("UAGENT_CONFIG_FILE", {}, "", ReloadPolicy::kRestartRequired,
                  Sensitivity::kPublic, "behaviour",
                  "replace both config-file locations"),
    registry::Str("UAGENT_DEBUG_LOG", {}, "", ReloadPolicy::kRestartRequired,
                  Sensitivity::kPublic, "behaviour",
                  "write a sensitive reconstructable JSONL trace"),
    registry::Str("UAGENT_USAGE_FILE", {}, "", ReloadPolicy::kRestartRequired,
                  Sensitivity::kPublic, "behaviour",
                  "append per-turn usage records to this path"),
    registry::Str("UAGENT_MARKDOWN", {}, "1", ReloadPolicy::kRestartRequired,
                  Sensitivity::kPublic, "behaviour",
                  "render Markdown on a TTY"),
    registry::Str("UAGENT_HEADLESS_PROGRESS", {}, "",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                  "behaviour", "echo progress lines in headless mode"),
    registry::Str("UAGENT_MEMORY_REDACT_KEYWORDS", {}, "",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                  "behaviour", "extra keywords redacted from stored memories"),
};

inline std::span<const ConfigDescriptor> ConfigRegistry() {
  return kConfigRegistry;
}

const ConfigDescriptor* FindConfigDescriptor(std::string_view environment);

// Declared, never defined, and deliberately not constexpr: naming an
// unregistered setting therefore fails to compile at the call site.
void UnregisteredConfigSetting();

// Compile-time descriptor lookup: a getter naming an unregistered setting
// fails to compile rather than inventing a default.
consteval const ConfigDescriptor& Cfg(std::string_view environment) {
  for (const ConfigDescriptor& descriptor : kConfigRegistry) {
    if (descriptor.environment == environment) return descriptor;
  }
  UnregisteredConfigSetting();
  return kConfigRegistry[0];
}

int64_t LongSetting(const ConfigDescriptor& descriptor);
// For the few settings whose default is derived from another setting; bounds
// and metadata still come from the descriptor.
int64_t LongSetting(const ConfigDescriptor& descriptor, int64_t fallback);
bool BoolSetting(const ConfigDescriptor& descriptor);
std::string StringSetting(const ConfigDescriptor& descriptor);

const char* ConfigTypeName(ConfigType type);
const char* ReloadPolicyName(ReloadPolicy policy);
const char* SensitivityName(Sensitivity sensitivity);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_CONFIG_REGISTRY_H_
