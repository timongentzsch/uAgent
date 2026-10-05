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
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "include/core/signals.h"

namespace uagent {

inline constexpr int64_t kConfigAnyMin = std::numeric_limits<int64_t>::min();
inline constexpr int64_t kConfigAnyMax = std::numeric_limits<int64_t>::max();
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
  // One conversation's own choice, kept with it.
  kScopeConversation = 1u << 2,
};

// Every scope a value can come from, lowest first, by the name a snapshot's
// sources use and the name people read. A flag or the environment decides
// for one process; the three persisted scopes are where a value is saved.
struct ConfigScopeName {
  std::string_view source;
  std::string_view label;
  unsigned persisted;  // its ConfigScope bit, or 0
};
inline constexpr ConfigScopeName kConfigScopes[] = {
    {"user", "All conversations", kScopeUser},
    {"project", "This project", kScopeProject},
    {"environment", "Environment", 0},
    {"cli", "Command line", 0},
    {"conversation", "This conversation", kScopeConversation},
};
// The groups settings are listed in, in the order and under the names people
// read: the settings screen and the generated reference both follow it.
struct ConfigCategory {
  std::string_view id;
  std::string_view label;
};
inline constexpr ConfigCategory kConfigCategories[] = {
    {"route", "Models and connection"},
    {"behaviour", "Behaviour"},
    {"budget", "Limits"},
    {"request", "Requests"},
    {"tools", "Tools"},
    {"delegation", "Subagents"},
    {"coordination", "Coordinator"},
    {"search", "Web search"},
    {"memory", "Memory"},
    {"skills", "Skills"},
    {"mcp", "MCP"},
    {"media", "Media"},
    {"retention", "History"},
    {"web", "Web host"},
};

// The name people read for a source; anything else is the default.
constexpr std::string_view ConfigScopeLabel(std::string_view source) {
  for (const ConfigScopeName& scope : kConfigScopes) {
    if (scope.source == source) return scope.label;
  }
  return "Default";
}

using ConfigDefault = std::variant<int64_t, double, bool, std::string_view>;

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
  // Accepted spellings of a fixed-choice string; empty means free text. An
  // empty value always means the default.
  std::span<const std::string_view> choices = {};
  // What applies while the value is empty, when the default cannot say it:
  // another setting's name, or a short phrase.
  std::string_view fallback = {};
  // A person's name for the setting and what it is for, in a sentence; the
  // web and the CLI show these instead of the variable name where set.
  std::string_view label = {};
  std::string_view purpose = {};
  bool terminal = false;

  bool Accepts(std::string_view value) const {
    if (choices.empty() || value.empty()) return true;
    for (std::string_view choice : choices) {
      if (value == choice) return true;
    }
    return false;
  }
};

inline constexpr std::string_view kOpenRouterVariants[] = {"nitro", "floor",
                                                           "exacto"};
inline constexpr std::string_view kReasoningEfforts[] = {
    "none", "minimal", "low", "medium", "high", "xhigh", "max"};
inline constexpr std::string_view kWebSearchBackends[] = {"auto", "openrouter",
                                                          "off"};
inline constexpr std::string_view kApprovalModes[] = {"ask", "auto", "yolo"};
inline constexpr std::string_view kThreadEnvironments[] = {"worktree", "local"};
inline constexpr std::string_view kVerbosityLevels[] = {"minimal", "default",
                                                        "full"};

// Default model route when nothing is configured: DeepSeek flash through
// OpenRouter auto-routing. One constant so the provider template and side-model
// defaults cannot drift apart.
inline constexpr const char* kDefaultModelRoute =
    "~deepseek/deepseek-flash-latest";

// Sent when a route has no credential; local OpenAI-compatible servers accept
// any bearer value, and "no key configured" checks compare against it.
inline constexpr char kPlaceholderApiKey[] = "sk-noop";

namespace registry {

// Shorthand keeps one row on one line so the table stays readable; every field
// after the bounds is spelled out at the call site.
consteval ConfigDescriptor Int(std::string_view env, std::string_view field,
                               int64_t value, int64_t minimum, int64_t maximum,
                               ReloadPolicy reload, std::string_view category,
                               std::string_view description,
                               unsigned scopes = kScopeUser | kScopeProject) {
  return {env,     field,    ConfigType::kInt, value,
          minimum, maximum,  reload,           Sensitivity::kPublic,
          scopes,  category, description};
}

consteval ConfigDescriptor Str(std::string_view env, std::string_view field,
                               std::string_view value, ReloadPolicy reload,
                               Sensitivity sensitivity,
                               std::string_view category,
                               std::string_view description,
                               unsigned scopes = kScopeUser | kScopeProject) {
  return {env,           field,  ConfigType::kString, value,  kConfigAnyMin,
          kConfigAnyMax, reload, sensitivity,         scopes, category,
          description};
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

consteval ConfigDescriptor Choice(ConfigDescriptor descriptor,
                                  std::span<const std::string_view> choices) {
  descriptor.choices = choices;
  return descriptor;
}

// An empty value follows another setting or a runtime choice.
consteval ConfigDescriptor Fallback(ConfigDescriptor descriptor,
                                    std::string_view fallback) {
  descriptor.fallback = fallback;
  return descriptor;
}

consteval ConfigDescriptor Named(ConfigDescriptor descriptor,
                                 std::string_view label,
                                 std::string_view purpose = {}) {
  descriptor.label = label;
  descriptor.purpose = purpose;
  return descriptor;
}

// One conversation may choose its own value, kept with it.
consteval ConfigDescriptor PerConversation(ConfigDescriptor descriptor) {
  descriptor.scopes |= kScopeConversation;
  return descriptor;
}

// Only a terminal process or a flag uses it; the web does not list it.
consteval ConfigDescriptor Terminal(ConfigDescriptor descriptor) {
  descriptor.terminal = true;
  return descriptor;
}

}  // namespace registry

inline constexpr ConfigDescriptor kConfigRegistry[] = {
    // The web master is per OS user, so its settings never come from a project.
    registry::Terminal(registry::Named(
        registry::Str(
            "UAGENT_WEB_BIND", {}, "127.0.0.1", ReloadPolicy::kRestartRequired,
            Sensitivity::kPublic, "web",
            "web listener address: loopback by default, all interfaces "
            "only when explicitly configured",
            kScopeUser),
        "Web listen address")),
    registry::Terminal(registry::Named(
        registry::Str("UAGENT_BROWSER_DATA", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "web",
                      "private browser profile and service directory; empty "
                      "disables the browser appliance",
                      kScopeUser),
        "Browser data folder")),
    registry::Terminal(registry::Named(
        registry::Int("UAGENT_WEB_PORT", {}, 8080, 1024, 65535,
                      ReloadPolicy::kRestartRequired, "web",
                      "global web master's loopback port", kScopeUser),
        "Web port")),
    registry::Terminal(registry::Named(
        registry::Str(
            "UAGENT_WEB_ORIGIN", {}, "", ReloadPolicy::kRestartRequired,
            Sensitivity::kPublic, "web",
            "exact browser origin via an explicitly configured HTTPS or "
            "tailnet proxy",
            kScopeUser),
        "Web origin")),
    registry::Named(
        registry::Str("UAGENT_WEB_PUSH_CONTACT", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "web",
                      "VAPID mailto or HTTPS contact; empty disables optional "
                      "native Web Push",
                      kScopeUser),
        "Push contact"),
    // Route selection and credentials.
    registry::Named(
        registry::Fallback(
            registry::Str("UAGENT_BASE_URL", {}, "",
                          ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                          "route", "active API base URL"),
            "OpenRouter when OPENROUTER_API_KEY is set"),
        "API address",
        "Where requests go; empty uses OpenRouter when its key is set"),
    registry::Named(
        registry::Str("UAGENT_API_KEY", {}, kPlaceholderApiKey,
                      ReloadPolicy::kRestartRequired, Sensitivity::kSecret,
                      "route", "credential for the active route"),
        "API key", "Credential sent to the API address"),
    registry::Named(
        registry::Str("OPENROUTER_API_KEY", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kSecret,
                      "route",
                      "OpenRouter credential used when no base URL is set"),
        "OpenRouter key",
        "Credential for OpenRouter, used when no API address is set"),
    registry::PerConversation(registry::Named(
        registry::Fallback(
            registry::Str(
                "UAGENT_MODEL", {}, "", ReloadPolicy::kRestartRequired,
                Sensitivity::kPublic, "route",
                "model or named route as [provider/]model[:variant][:effort]"),
            "the provider default"),
        "Conversation model",
        "Answers your messages; /model chooses it for one conversation")),
    registry::Named(
        registry::Fallback(
            registry::Choice(
                registry::Str(
                    "UAGENT_REASONING_EFFORT",
                    {}, "", ReloadPolicy::kRestartRequired,
                    Sensitivity::kPublic,
                    "route", "none, minimal, low, medium, high, xhigh, or max"),
                kReasoningEfforts),
            "provider default"),
        "Reasoning effort",
        "How long the conversation model thinks before answering"),
    registry::Named(
        registry::Str(
            "UAGENT_PROVIDERS", {}, "", ReloadPolicy::kRestartRequired,
            Sensitivity::kCompositeSecret, "route",
            "JSON object of named endpoints, transports, and aliases"),
        "Named providers"),
    registry::Named(
        registry::Str("UAGENT_OPENROUTER_PROVIDER", "openrouter_provider", "",
                      ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                      "route", "pin OpenRouter to one upstream provider"),
        "OpenRouter provider"),
    registry::Named(
        registry::Choice(
            registry::Str("UAGENT_OPENROUTER_VARIANT", "openrouter_variant", "",
                          ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                          "route",
                          "nitro, floor, or exacto routing preference"),
            kOpenRouterVariants),
        "OpenRouter routing"),
    registry::Named(
        registry::Int("UAGENT_CONTEXT", {}, 0, kConfigAnyMin, kConfigAnyMax,
                      ReloadPolicy::kRestartRequired, "route",
                      "context-window tokens; 0 uses the provider profile"),
        "Context window"),
    registry::Named(
        registry::Int("UAGENT_MAX_TOKENS", {}, -1, kConfigAnyMin, kConfigAnyMax,
                      ReloadPolicy::kNextUserTurn, "route",
                      "maximum response tokens; -1 omits the optional cap"),
        "Response length"),

    // Request transport.
    registry::Named(
        registry::Int(
            "UAGENT_STREAM_TIMEOUT", "stream_timeout_s", 300, kConfigAnyMin,
            kConfigAnyMax, ReloadPolicy::kNextUserTurn, "request",
            "seconds of stream silence allowed, before the first event "
            "or between events"),
        "Stream timeout"),
    registry::Named(
        registry::Int("UAGENT_REQUEST_TIMEOUT", "request_timeout_s", 600,
                      kConfigAnyMin, kConfigAnyMax, ReloadPolicy::kNextUserTurn,
                      "request", "total seconds allowed for one model request"),
        "Request timeout"),

    // Turn budgets.
    registry::Named(
        registry::Int("UAGENT_MAX_STEPS", "max_steps", 0, 0, kConfigAnyMax,
                      ReloadPolicy::kNextUserTurn, "budget",
                      "model rounds per turn; 0 disables the limit"),
        "Steps per turn"),
    registry::Named(
        registry::Int("UAGENT_MAX_TOOL_CALLS", "max_tool_calls", 0, 0,
                      kConfigAnyMax, ReloadPolicy::kNextUserTurn, "budget",
                      "tool calls per turn; 0 disables the limit"),
        "Tool calls per turn"),
    registry::Named(
        registry::Int("UAGENT_MAX_TURN_SECONDS", "max_turn_seconds", 0, 0,
                      kConfigAnyMax, ReloadPolicy::kNextUserTurn, "budget",
                      "wall-clock seconds per turn; 0 disables the deadline"),
        "Turn time limit"),
    registry::Named(
        registry::Int("UAGENT_MAX_TURN_TOKENS", "max_turn_tokens", 0, 0,
                      kConfigAnyMax, ReloadPolicy::kNextUserTurn, "budget",
                      "generated-token ceiling per turn; 0 disables it"),
        "Tokens per turn"),
    registry::Named(
        registry::Int("UAGENT_SESSION_TOKEN_BUDGET", "session_token_budget", 0,
                      0, kConfigAnyMax, ReloadPolicy::kNextUserTurn, "budget",
                      "cumulative generated-token ceiling; 0 disables it"),
        "Conversation token budget"),
    registry::Named(
        registry::Dbl("UAGENT_MAX_TURN_COST", "max_turn_cost", 0.0,
                      ReloadPolicy::kNextUserTurn, "budget",
                      "reported-cost ceiling per turn; 0 disables it"),
        "Cost per turn"),
    registry::Named(
        registry::Dbl("UAGENT_SESSION_BUDGET", "session_budget", 0.0,
                      ReloadPolicy::kNextUserTurn, "budget",
                      "cumulative reported-cost ceiling; 0 disables it"),
        "Conversation budget"),
    registry::Named(registry::Int("UAGENT_TOOL_TIMEOUT", "tool_timeout_s", 30,
                                  0, kConfigAnyMax, ReloadPolicy::kNextUserTurn,
                                  "budget", "seconds one tool call may run"),
                    "Tool timeout"),
    registry::Named(
        registry::Int("UAGENT_AUTO_COMPACT_PCT", {}, 85, kConfigAnyMin,
                      kConfigAnyMax, ReloadPolicy::kNextUserTurn, "budget",
                      "context percentage that triggers compaction"),
        "Compact at"),

    // Tool results and trace retention.
    registry::Named(
        registry::Bul("UAGENT_PRUNE_IN_TURN", {}, false,
                      ReloadPolicy::kNextUserTurn, "tools",
                      "shorten old tool results while a long turn still "
                      "runs: less input, at the risk of reading again"),
        "Prune inside a turn"),
    registry::Named(
        registry::Int("UAGENT_TOOL_RESULT_CHARS", {}, 8000, kConfigAnyMin,
                      kConfigAnyMax, ReloadPolicy::kNextUserTurn, "tools",
                      "characters kept from one tool result"),
        "Tool result size"),
    registry::Named(
        registry::Int("UAGENT_READ_FILE_LINES", {}, 1000, kConfigAnyMin,
                      kConfigAnyMax, ReloadPolicy::kNextUserTurn, "tools",
                      "default lines returned by read_path"),
        "Lines per file read"),

    // OS sandbox for agent-run commands. Restart-required because the policy is
    // built once and every spawn is wrapped with it; a mid-session change would
    // leave already-running jobs under the old confinement.
    registry::Named(registry::Bul("UAGENT_SANDBOX", {}, true,
                                  ReloadPolicy::kRestartRequired, "tools",
                                  "confine shell commands with the OS sandbox"),
                    "Sandbox"),
    registry::Named(registry::Bul("UAGENT_SANDBOX_NET", {}, true,
                                  ReloadPolicy::kRestartRequired, "tools",
                                  "let sandboxed commands reach the network"),
                    "Sandbox network access"),
    registry::Named(
        registry::Str("UAGENT_SANDBOX_WRITE", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "tools",
                      "extra writable roots for the sandbox, colon-separated"),
        "Sandbox writable folders"),

    // Delegation.
    registry::Named(
        registry::Int("UAGENT_SUBAGENT_DEPTH", {}, 2, 0, kConfigAnyMax,
                      ReloadPolicy::kRestartRequired, "delegation",
                      "deepest delegation level allowed"),
        "Delegation depth"),
    registry::Named(
        registry::Int("UAGENT_SUBAGENT_MAX_STEPS", {}, 100, kConfigAnyMin,
                      kConfigAnyMax, ReloadPolicy::kNextUserTurn, "delegation",
                      "model rounds per delegated child"),
        "Sub-agent steps"),
    registry::Named(
        registry::Int("UAGENT_SUBAGENT_MAX_TOOL_CALLS", {}, 240, kConfigAnyMin,
                      kConfigAnyMax, ReloadPolicy::kNextUserTurn, "delegation",
                      "tool calls per delegated child"),
        "Sub-agent tool calls"),
    registry::Named(
        registry::Int("UAGENT_SUBAGENT_TIMEOUT", {}, 0, 0, kConfigAnyMax,
                      ReloadPolicy::kNextUserTurn, "delegation",
                      "wall-clock ceiling per delegated child; 0 is the turn"),
        "Sub-agent time limit"),
    registry::Named(
        registry::Fallback(
            registry::Str("UAGENT_SUBAGENT_MODEL", {}, "",
                          ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                          "delegation",
                          "default model route for delegated children"),
            "UAGENT_MODEL"),
        "Sub-agent model", "Runs the side tasks a conversation delegates"),

    // Coordination.
    registry::Named(
        registry::Fallback(
            registry::Str("UAGENT_COORDINATOR_MODEL", {}, "",
                          ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                          "coordination",
                          "model route of each folder's coordinator; /model "
                          "inside it overrides this for that folder"),
            "UAGENT_MODEL"),
        "Coordinator model",
        "Plans and supervises a folder's threads; /model inside a coordinator "
        "overrides it there"),
    registry::Named(
        registry::Int("UAGENT_COORDINATOR_MAX_THREADS", {}, 5, 1, 64,
                      ReloadPolicy::kNextUserTurn, "coordination",
                      "threads one coordinator may run at once"),
        "Coordinator threads"),
    registry::Named(
        registry::Dbl(
            "UAGENT_COORDINATOR_DAILY_SPEND_USD", {}, 20.0,
            ReloadPolicy::kNextUserTurn, "coordination",
            "reported cost a coordinator and its threads may spend per "
            "day; at it, thread events wait. 0 disables it"),
        "Coordinator daily spend"),
    registry::Named(
        registry::Choice(
            registry::Str("UAGENT_COORDINATOR_ENVIRONMENT", {}, "worktree",
                          ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                          "coordination",
                          "where threads run: a fresh git worktree, or the "
                          "folder itself"),
            kThreadEnvironments),
        "Where threads run"),

    // Web search.
    registry::Named(
        registry::Choice(
            registry::Str("UAGENT_WEB_SEARCH_BACKEND", "web_search_backend",
                          "auto", ReloadPolicy::kRestartRequired,
                          Sensitivity::kPublic, "search",
                          "auto, openrouter, or off"),
            kWebSearchBackends),
        "Web search"),
    registry::Named(
        registry::Fallback(
            registry::Str("UAGENT_WEB_SEARCH_MODEL", "web_search_model", "",
                          ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                          "search", "model route used for search"),
            "conversation model on OpenRouter, else the default route"),
        "Web search model", "Answers web lookups"),

    // Memory.
    registry::Named(registry::Bul("UAGENT_MEMORY", "memory_enabled", true,
                                  ReloadPolicy::kRestartRequired, "memory",
                                  "enable memory recall and writes"),
                    "Memory"),
    registry::Named(
        registry::Bul("UAGENT_MEMORY_GENERATE", "memory_generate", true,
                      ReloadPolicy::kRestartRequired, "memory",
                      "run the background memory extractor"),
        "Memory extraction"),
    registry::Named(
        registry::Fallback(
            registry::Str("UAGENT_MEMORY_MODEL", {}, "",
                          ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                          "memory",
                          "model route for background memory extraction"),
            "UAGENT_MODEL"),
        "Memory model",
        "Distils memories from finished conversations in the background"),

    // Skills.
    registry::Terminal(registry::Named(
        registry::Str("UAGENT_SKILL_PATH", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "skills", "replace the entire skill search path"),
        "Skill folders")),
    registry::Named(
        registry::Str("UAGENT_SKILL_EXCLUDE", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "skills", "comma-separated skill names to withhold"),
        "Disabled skills"),
    registry::Named(
        registry::Str("UAGENT_OTHER_AGENTS", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "skills",
                      "also read what these agents keep (skills, memories, "
                      "CLAUDE.md): claude, codex, comma-separated; empty "
                      "reads none"),
        "Other agents"),

    // MCP.
    registry::Named(registry::Int("UAGENT_MCP_TIMEOUT", "mcp_timeout_s", 60, 1,
                                  kConfigAnyMax, ReloadPolicy::kRestartRequired,
                                  "mcp", "seconds allowed for one MCP call"),
                    "MCP call timeout"),
    registry::Named(
        registry::Str("UAGENT_MCP_ROOTS", "mcp_roots", "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "mcp", "roots advertised to MCP servers"),
        "MCP roots"),

    // Attachments and terminal media.
    registry::Named(
        registry::Fallback(
            registry::Str(
                "UAGENT_IMAGE_MODEL",
                "image_model", "",
                ReloadPolicy::kNextUserTurn, Sensitivity::kPublic, "media",
                "model route that reads attached images; empty uses the "
                "main route when it reads images, else the shared "
                "default route"),
            "main route if it reads images, else the default route"),
        "Image reader",
        "Describes attached images when the conversation model cannot see "
        "them"),
    registry::Named(
        registry::Str("UAGENT_PDF_ENGINE", "pdf_engine", "cloudflare-ai",
                      ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                      "media", "OpenRouter file-parser engine for documents"),
        "PDF reader"),
    registry::Named(
        registry::Int("UAGENT_ATTACHMENT_MB", {}, 10, 1, kConfigMaxMegabytes,
                      ReloadPolicy::kNextUserTurn, "media",
                      "largest attachment in mebibytes"),
        "Attachment size limit"),
    // Session and artifact retention.
    registry::Named(registry::Int("UAGENT_HISTORY_DAYS", {}, 30, kConfigAnyMin,
                                  kConfigAnyMax, ReloadPolicy::kNextUserTurn,
                                  "retention", "days of saved sessions kept"),
                    "History kept"),

    // Behaviour switches.
    registry::Named(
        registry::Bul(
            "UAGENT_ADAPT_SYSTEM", {}, false, ReloadPolicy::kRestartRequired,
            "behaviour",
            "expose adapt_system so the model may revise its directive"),
        "Self-directive tool"),
    registry::PerConversation(registry::Named(
        registry::Choice(
            registry::Str("UAGENT_APPROVAL", "approval", "ask",
                          ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                          "behaviour",
                          "ask, auto reviewer, or yolo for ordinary mutations"),
            kApprovalModes),
        "Approval mode")),
    registry::Named(
        registry::Str("UAGENT_PERMISSION_MODEL", "permission_model",
                      "~typesafe/jev-latest", ReloadPolicy::kNextUserTurn,
                      Sensitivity::kPublic, "behaviour",
                      "OpenRouter Decisions model used by auto permissions"),
        "Permission reviewer", "Judges risky actions when approval is Auto"),
    registry::Terminal(registry::Named(
        registry::Str("UAGENT_PERMISSION_URL", "permission_url",
                      "https://openrouter.ai/api/alpha",
                      ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                      "behaviour", "OpenRouter Decisions API base URL"),
        "Permission reviewer address")),
    registry::Named(
        registry::Str("UAGENT_TITLE_MODEL", "title_model", kDefaultModelRoute,
                      ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                      "behaviour",
                      "model route that names new sessions, or off"),
        "Title model",
        "Names new conversations; off keeps the first message as the title"),
    registry::Named(
        registry::Str("UAGENT_TOOL_CAPABILITIES", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "behaviour", "restrict the exposed tool capability set"),
        "Tool capabilities"),
    registry::Named(
        registry::Str(
            "UAGENT_SHELL_ENV_ALLOW", {}, "",
            ReloadPolicy::kNextUserTurn, Sensitivity::kPublic, "behaviour",
            "comma-separated sensitive variables approved shells may inherit"),
        "Shell variables allowed"),
    registry::Terminal(registry::Named(
        registry::Bul("UAGENT_TRUST_PROJECT_CONFIG", {}, false,
                      ReloadPolicy::kRestartRequired, "behaviour",
                      "trust this workspace's .mcp.json"),
        "Trust project config")),
    registry::Terminal(registry::Named(
        registry::Str("UAGENT_DEBUG_LOG", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "behaviour",
                      "write a sensitive reconstructable JSONL trace"),
        "Debug log")),
    registry::Terminal(
        registry::Named(registry::Bul("UAGENT_MARKDOWN", {}, true,
                                      ReloadPolicy::kRestartRequired,
                                      "behaviour", "render Markdown on a TTY"),
                        "Render Markdown")),
    registry::Terminal(registry::Named(
        registry::Bul("UAGENT_PLAIN", {}, false, ReloadPolicy::kRestartRequired,
                      "behaviour",
                      "screen-reader terminal: labelled lines, no animation or "
                      "cursor control"),
        "Plain output")),
    registry::Terminal(registry::Named(
        registry::Bul("UAGENT_REDUCED_MOTION", {}, false,
                      ReloadPolicy::kRestartRequired, "behaviour",
                      "show a still status instead of the terminal spinner"),
        "Reduced motion")),
    registry::Named(
        registry::Choice(
            registry::Str("UAGENT_VERBOSITY", {}, "default",
                          ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                          "behaviour",
                          "how much of the agent's work is shown: minimal, "
                          "default or full; display only"),
            kVerbosityLevels),
        "Detail shown"),
    registry::Named(
        registry::Str("UAGENT_MEMORY_REDACT_KEYWORDS", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "behaviour",
                      "extra keywords redacted from stored memories"),
        "Memory redaction keywords"),
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

// A session's settings, apart from environ, which is never rewritten after
// startup. Lookup order: override, published configuration, the route in
// use, environ.
using SettingValues = std::map<std::string, std::string>;
void PublishSettings(SettingValues values);
// The route in use, below what is configured: what a command started from
// here is told where the configuration names nothing. It is no part of this
// process's environment, which the runtimes it starts inherit as it was.
void PublishRoute(SettingValues values);
void OverrideSetting(std::string_view environment, std::string value);
void ClearSettings();
SettingValues CurrentSettings();
std::string SettingText(const ConfigDescriptor& descriptor);
// The same by name, for the few names that are no registered setting (a
// provider's own variables). What is saved is not in the environment: it is
// read here.
std::string SettingText(const std::string& name);

int64_t LongSetting(const ConfigDescriptor& descriptor);
bool BoolSetting(const ConfigDescriptor& descriptor);
std::string StringSetting(const ConfigDescriptor& descriptor);
double DoubleSetting(const ConfigDescriptor& descriptor);

const char* ConfigTypeName(ConfigType type);
const char* ReloadPolicyName(ReloadPolicy policy);
const char* SensitivityName(Sensitivity sensitivity);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_CONFIG_REGISTRY_H_
