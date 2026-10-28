// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_CONFIG_REGISTRY_H_
#define UAGENT_INCLUDE_CORE_CONFIG_REGISTRY_H_
// The authoritative description of every UAGENT_* setting: default, bounds,
// when a change takes effect, and whether the value may be shown. Runtime
// getters, RuntimeConfig and the self-description tool all read these
// descriptors, so a documented default cannot drift from the one the binary
// applies.
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
// read: the settings screen and `uagent inspect` both follow it.
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
    {"coordination", "Panel"},
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
  // The setting's name in the settings file: "limits.maxSteps".
  std::string_view key = {};

  bool Accepts(std::string_view value) const {
    if (choices.empty() || value.empty()) return true;
    for (std::string_view choice : choices) {
      if (value == choice) return true;
    }
    return false;
  }
};

inline constexpr std::string_view kReasoningEfforts[] = {
    "none", "minimal", "low", "medium", "high", "xhigh", "max"};
inline constexpr std::string_view kWebSearchBackends[] = {"auto", "off"};
inline constexpr std::string_view kApprovalModes[] = {"ask", "auto", "yolo"};
inline constexpr std::string_view kSandboxLevels[] = {
    "readonly", "strict", "default", "relaxed", "off"};
inline constexpr std::string_view kThreadEnvironments[] = {"worktree", "local"};
inline constexpr std::string_view kVerbosityLevels[] = {"minimal", "default",
                                                        "full"};

// Sent when a route has no credential; local OpenAI-compatible servers accept
// any bearer value, and "no key configured" checks compare against it.
inline constexpr char kPlaceholderApiKey[] = "sk-noop";

namespace registry {

// Every row names its setting twice: as the settings file spells it, and as
// the environment does.
consteval ConfigDescriptor Keyed(std::string_view key,
                                 ConfigDescriptor descriptor) {
  descriptor.key = key;
  return descriptor;
}

// Shorthand keeps one row on one line so the table stays readable; every field
// after the bounds is spelled out at the call site.
consteval ConfigDescriptor Int(std::string_view key, std::string_view env,
                               std::string_view field, int64_t value,
                               int64_t minimum, int64_t maximum,
                               ReloadPolicy reload, std::string_view category,
                               std::string_view description,
                               unsigned scopes = kScopeUser | kScopeProject) {
  return Keyed(key,
               {env, field, ConfigType::kInt, value, minimum, maximum, reload,
                Sensitivity::kPublic, scopes, category, description});
}

consteval ConfigDescriptor Str(std::string_view key, std::string_view env,
                               std::string_view field, std::string_view value,
                               ReloadPolicy reload, Sensitivity sensitivity,
                               std::string_view category,
                               std::string_view description,
                               unsigned scopes = kScopeUser | kScopeProject) {
  return Keyed(
      key, {env, field, ConfigType::kString, value, kConfigAnyMin,
            kConfigAnyMax, reload, sensitivity, scopes, category, description});
}

consteval ConfigDescriptor Bul(std::string_view key, std::string_view env,
                               std::string_view field, bool value,
                               ReloadPolicy reload, std::string_view category,
                               std::string_view description) {
  return Keyed(key, {env, field, ConfigType::kBool, value, kConfigAnyMin,
                     kConfigAnyMax, reload, Sensitivity::kPublic,
                     kScopeUser | kScopeProject, category, description});
}

consteval ConfigDescriptor Dbl(std::string_view key, std::string_view env,
                               std::string_view field, double value,
                               ReloadPolicy reload, std::string_view category,
                               std::string_view description) {
  return Keyed(key, {env, field, ConfigType::kDouble, value, kConfigAnyMin,
                     kConfigAnyMax, reload, Sensitivity::kPublic,
                     kScopeUser | kScopeProject, category, description});
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
    // The web host is per OS user, so its settings never come from a project.
    registry::Terminal(registry::Named(
        registry::Str(
            "web.bind", "UAGENT_WEB_BIND", {}, "127.0.0.1",
            ReloadPolicy::kRestartRequired, Sensitivity::kPublic, "web",
            "web listener address: loopback by default, all interfaces "
            "only when explicitly configured",
            kScopeUser),
        "Web listen address")),
    registry::Terminal(registry::Named(
        registry::Str("web.browserData", "UAGENT_BROWSER_DATA", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "web",
                      "private browser profile and service directory; empty "
                      "disables the shared browser",
                      kScopeUser),
        "Browser data folder")),
    registry::Terminal(registry::Named(
        registry::Int("web.port", "UAGENT_WEB_PORT", {}, 8080, 1024, 65535,
                      ReloadPolicy::kRestartRequired, "web",
                      "the web host's loopback port", kScopeUser),
        "Web port")),
    registry::Terminal(registry::Named(
        registry::Str(
            "web.origin", "UAGENT_WEB_ORIGIN", {}, "",
            ReloadPolicy::kRestartRequired, Sensitivity::kPublic, "web",
            "exact browser origin via an explicitly configured HTTPS or "
            "tailnet proxy",
            kScopeUser),
        "Web origin")),
    registry::Named(
        registry::Str("web.pushContact", "UAGENT_WEB_PUSH_CONTACT", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "web",
                      "VAPID mailto or HTTPS contact; empty disables optional "
                      "native Web Push",
                      kScopeUser),
        "Push contact"),
    // Route selection and credentials.
    registry::Named(
        registry::Fallback(
            registry::Str("endpoint.baseUrl", "UAGENT_BASE_URL", {}, "",
                          ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                          "route", "active API base URL"),
            "OpenRouter when OPENROUTER_API_KEY is set"),
        "API address",
        "Where requests go; empty uses OpenRouter when its key is set"),
    registry::Named(
        registry::Str("endpoint.apiKey", "UAGENT_API_KEY", {},
                      kPlaceholderApiKey, ReloadPolicy::kRestartRequired,
                      Sensitivity::kSecret, "route",
                      "credential for the active route"),
        "API key", "Credential sent to the API address"),
    registry::Named(
        registry::Str("openrouter.apiKey", "OPENROUTER_API_KEY", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kSecret,
                      "route",
                      "OpenRouter credential used when no base URL is set"),
        "OpenRouter key",
        "Credential for OpenRouter, used when no API address is set"),
    registry::PerConversation(registry::Named(
        registry::Fallback(
            registry::Str(
                "model", "UAGENT_MODEL",
                {}, "", ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                "route",
                "model or named route as [provider/]model[:variant][:effort]"),
            "the provider default"),
        "Conversation model",
        "Answers your messages; /model chooses it for one conversation")),
    registry::Named(
        registry::Fallback(
            registry::Choice(
                registry::Str(
                    "reasoningEffort", "UAGENT_REASONING_EFFORT", {}, "",
                    ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                    "route", "none, minimal, low, medium, high, xhigh, or max"),
                kReasoningEfforts),
            "provider default"),
        "Reasoning effort",
        "How long the conversation model thinks before answering"),
    registry::Named(
        registry::Str(
            "providers", "UAGENT_PROVIDERS", {}, "",
            ReloadPolicy::kRestartRequired, Sensitivity::kCompositeSecret,
            "route", "JSON object of named endpoints, transports, and aliases"),
        "Named providers"),
    registry::Named(
        registry::Int("contextWindow", "UAGENT_CONTEXT", {}, 0, kConfigAnyMin,
                      kConfigAnyMax, ReloadPolicy::kRestartRequired, "route",
                      "context-window tokens; 0 uses the provider profile"),
        "Context window"),
    registry::Named(
        registry::Int("maxOutputTokens", "UAGENT_MAX_TOKENS", {}, -1,
                      kConfigAnyMin, kConfigAnyMax, ReloadPolicy::kNextUserTurn,
                      "route",
                      "maximum response tokens; -1 omits the optional cap"),
        "Response length"),

    // Request transport.
    registry::Named(
        registry::Int(
            "request.streamTimeout", "UAGENT_STREAM_TIMEOUT",
            "stream_timeout_s", 300, kConfigAnyMin, kConfigAnyMax,
            ReloadPolicy::kNextUserTurn, "request",
            "seconds of stream silence allowed, before the first event "
            "or between events"),
        "Stream timeout"),
    registry::Named(
        registry::Int("request.timeout", "UAGENT_REQUEST_TIMEOUT",
                      "request_timeout_s", 600, kConfigAnyMin, kConfigAnyMax,
                      ReloadPolicy::kNextUserTurn, "request",
                      "total seconds allowed for one model request"),
        "Request timeout"),

    // Turn budgets.
    registry::Named(
        registry::Int("limits.maxSteps", "UAGENT_MAX_STEPS", "max_steps", 0, 0,
                      kConfigAnyMax, ReloadPolicy::kNextUserTurn, "budget",
                      "model rounds per turn; 0 disables the limit"),
        "Steps per turn"),
    registry::Named(
        registry::Int("limits.maxToolCalls", "UAGENT_MAX_TOOL_CALLS",
                      "max_tool_calls", 0, 0, kConfigAnyMax,
                      ReloadPolicy::kNextUserTurn, "budget",
                      "tool calls per turn; 0 disables the limit"),
        "Tool calls per turn"),
    registry::Named(
        registry::Int("limits.maxTurnSeconds", "UAGENT_MAX_TURN_SECONDS",
                      "max_turn_seconds", 0, 0, kConfigAnyMax,
                      ReloadPolicy::kNextUserTurn, "budget",
                      "wall-clock seconds per turn; 0 disables the deadline"),
        "Turn time limit"),
    registry::Named(
        registry::Int("limits.maxTurnTokens", "UAGENT_MAX_TURN_TOKENS",
                      "max_turn_tokens", 0, 0, kConfigAnyMax,
                      ReloadPolicy::kNextUserTurn, "budget",
                      "generated-token ceiling per turn; 0 disables it"),
        "Tokens per turn"),
    registry::Named(
        registry::Int("limits.sessionTokens", "UAGENT_SESSION_TOKEN_BUDGET",
                      "session_token_budget", 0, 0, kConfigAnyMax,
                      ReloadPolicy::kNextUserTurn, "budget",
                      "cumulative generated-token ceiling; 0 disables it"),
        "Conversation token budget"),
    registry::Named(
        registry::Dbl("limits.maxTurnCost", "UAGENT_MAX_TURN_COST",
                      "max_turn_cost", 0.0, ReloadPolicy::kNextUserTurn,
                      "budget",
                      "reported-cost ceiling per turn; 0 disables it"),
        "Cost per turn"),
    registry::Named(
        registry::Dbl("limits.sessionCost", "UAGENT_SESSION_BUDGET",
                      "session_budget", 0.0, ReloadPolicy::kNextUserTurn,
                      "budget",
                      "cumulative reported-cost ceiling; 0 disables it"),
        "Conversation budget"),
    registry::Named(
        registry::Int("tools.timeout", "UAGENT_TOOL_TIMEOUT", "tool_timeout_s",
                      30, 0, kConfigAnyMax, ReloadPolicy::kNextUserTurn,
                      "budget", "seconds one tool call may run"),
        "Tool timeout"),
    registry::Named(
        registry::Int("context.autoCompactPercent", "UAGENT_AUTO_COMPACT_PCT",
                      {}, 85, kConfigAnyMin, kConfigAnyMax,
                      ReloadPolicy::kNextUserTurn, "budget",
                      "context percentage that triggers compaction"),
        "Compact at"),

    // Tool results and trace retention.
    registry::Named(
        registry::Int("tools.resultChars", "UAGENT_TOOL_RESULT_CHARS", {}, 8000,
                      kConfigAnyMin, kConfigAnyMax, ReloadPolicy::kNextUserTurn,
                      "tools", "characters kept from one tool result"),
        "Tool result size"),
    registry::Named(
        registry::Int("tools.readFileLines", "UAGENT_READ_FILE_LINES", {}, 1000,
                      kConfigAnyMin, kConfigAnyMax, ReloadPolicy::kNextUserTurn,
                      "tools", "default lines returned by read_path"),
        "Lines per file read"),

    // OS sandbox for agent-run commands. Restart-required because the policy is
    // built once and every spawn is wrapped with it; a mid-session change would
    // leave already-running jobs under the old confinement.
    registry::Named(
        registry::Choice(
            registry::Str("sandbox.level", "UAGENT_SANDBOX", {}, "default",
                          ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                          "tools",
                          "how far the OS sandbox confines shell commands: "
                          "readonly (temporary files only, no network; a "
                          "lead agent's always), strict (no network), "
                          "default, relaxed (home writable and readable) or "
                          "off; below relaxed, commands read in your home "
                          "folder only the workspace and their tools' folders"),
            kSandboxLevels),
        "Sandbox"),
    registry::Named(registry::Bul("sandbox.network", "UAGENT_SANDBOX_NET", {},
                                  true, ReloadPolicy::kRestartRequired, "tools",
                                  "let sandboxed commands reach the network"),
                    "Sandbox network access"),
    registry::Named(
        registry::Str("sandbox.write", "UAGENT_SANDBOX_WRITE", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "tools",
                      "extra writable roots for the sandbox, colon-separated"),
        "Sandbox writable folders"),
    registry::Named(
        registry::Str("sandbox.read", "UAGENT_SANDBOX_READ", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "tools",
                      "folders in your home folder that commands and file "
                      "tools may read without asking, colon-separated"),
        "Sandbox readable folders"),
    registry::Named(
        registry::Str("sandbox.hide", "UAGENT_SANDBOX_HIDE", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "tools",
                      "more paths the sandbox hides from commands and file "
                      "tools, colon-separated"),
        "Sandbox hidden paths"),
    registry::Named(
        registry::Str("sandbox.hideEnv", "UAGENT_SANDBOX_HIDE_ENV", {}, "",
                      ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                      "tools",
                      "more environment variables withheld from commands, MCP "
                      "servers and delegated agents, comma-separated"),
        "Hidden variables"),
    registry::Named(
        registry::Str("sandbox.reveal", "UAGENT_SANDBOX_REVEAL", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "tools",
                      "paths hidden by default that commands and file tools "
                      "may read, colon-separated"),
        "Sandbox revealed paths"),

    // Delegation.
    registry::Named(
        registry::Int("subagent.depth", "UAGENT_SUBAGENT_DEPTH", {}, 2, 0,
                      kConfigAnyMax, ReloadPolicy::kRestartRequired,
                      "delegation", "deepest delegation level allowed"),
        "Delegation depth"),
    registry::Named(
        registry::Str("subagent.model", "UAGENT_SUBAGENT_MODEL", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "delegation",
                      "route a subagent runs on when its call names none; "
                      "empty runs it on its parent's"),
        "Sub-agent model"),
    registry::Named(
        registry::Int("subagent.maxSteps", "UAGENT_SUBAGENT_MAX_STEPS", {}, 100,
                      kConfigAnyMin, kConfigAnyMax, ReloadPolicy::kNextUserTurn,
                      "delegation", "model rounds per delegated child"),
        "Sub-agent steps"),
    registry::Named(
        registry::Int("subagent.maxToolCalls", "UAGENT_SUBAGENT_MAX_TOOL_CALLS",
                      {}, 240, kConfigAnyMin, kConfigAnyMax,
                      ReloadPolicy::kNextUserTurn, "delegation",
                      "tool calls per delegated child"),
        "Sub-agent tool calls"),
    registry::Named(
        registry::Int("subagent.timeout", "UAGENT_SUBAGENT_TIMEOUT", {}, 0, 0,
                      kConfigAnyMax, ReloadPolicy::kNextUserTurn, "delegation",
                      "wall-clock ceiling per delegated child; 0 is the turn"),
        "Sub-agent time limit"),

    // Coordination.
    registry::Named(
        registry::Int("panel.maxThreads", "UAGENT_PANEL_MAX_THREADS", {}, 5, 1,
                      64, ReloadPolicy::kNextUserTurn, "coordination",
                      "background agents one lead agent may run at once"),
        "Lead agent: background agents"),
    registry::Named(
        registry::Dbl("panel.dailySpendUsd", "UAGENT_PANEL_DAILY_SPEND_USD", {},
                      20.0, ReloadPolicy::kNextUserTurn, "coordination",
                      "reported cost a lead agent and its background agents "
                      "may spend per day; at it, their events wait. 0 "
                      "disables it"),
        "Lead agent daily spend"),
    registry::Named(
        registry::Choice(
            registry::Str(
                "panel.environment", "UAGENT_PANEL_ENVIRONMENT", {}, "worktree",
                ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                "coordination",
                "where background agents run: a fresh git worktree, or "
                "the "
                "folder itself; a folder outside git runs in place"),
            kThreadEnvironments),
        "Where background agents run"),

    // Web search.
    registry::Named(
        registry::Choice(
            registry::Str("webSearch.backend", "UAGENT_WEB_SEARCH_BACKEND",
                          "web_search_backend", "auto",
                          ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                          "search", "auto or off"),
            kWebSearchBackends),
        "Web search"),
    registry::Named(
        registry::Fallback(
            registry::Str("webSearch.model", "UAGENT_WEB_SEARCH_MODEL",
                          "web_search_model", "", ReloadPolicy::kNextUserTurn,
                          Sensitivity::kPublic, "search",
                          "model route used for search"),
            "the conversation model, on an endpoint that offers search"),
        "Web search model", "Answers web lookups"),

    // Memory.
    registry::Named(
        registry::Bul("memory.enabled", "UAGENT_MEMORY", "memory_enabled", true,
                      ReloadPolicy::kRestartRequired, "memory",
                      "enable memory recall and writes"),
        "Memory"),
    registry::Named(
        registry::Bul("memory.generate", "UAGENT_MEMORY_GENERATE",
                      "memory_generate", true, ReloadPolicy::kRestartRequired,
                      "memory", "run the background memory extractor"),
        "Memory extraction"),
    registry::Named(
        registry::Fallback(
            registry::Str("memory.model", "UAGENT_MEMORY_MODEL", {}, "",
                          ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                          "memory",
                          "model route for background memory extraction"),
            "UAGENT_MODEL"),
        "Memory model",
        "Distils memories from finished conversations in the background"),

    // Skills.
    registry::Terminal(registry::Named(
        registry::Str("skills.path", "UAGENT_SKILL_PATH", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "skills", "replace the entire skill search path"),
        "Skill folders")),
    registry::Named(
        registry::Str("skills.exclude", "UAGENT_SKILL_EXCLUDE", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "skills", "comma-separated skill names to withhold"),
        "Disabled skills"),
    registry::Named(
        registry::Str("skills.otherAgents", "UAGENT_OTHER_AGENTS", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "skills",
                      "also read what these agents keep (skills, memories, "
                      "CLAUDE.md): claude, codex, comma-separated; empty "
                      "reads none"),
        "Other agents"),

    // MCP.
    registry::Named(
        registry::Int("mcp.timeout", "UAGENT_MCP_TIMEOUT", "mcp_timeout_s", 60,
                      1, kConfigAnyMax, ReloadPolicy::kRestartRequired, "mcp",
                      "seconds allowed for one MCP call"),
        "MCP call timeout"),

    // Attachments and terminal media.
    registry::Named(
        registry::Fallback(
            registry::Str(
                "image.model", "UAGENT_IMAGE_MODEL",
                "image_model", "", ReloadPolicy::kNextUserTurn,
                Sensitivity::kPublic, "media",
                "model route that reads attached images; empty uses the "
                "main route when it reads images, else the first model on "
                "its endpoint that does"),
            "main route if it reads images, else a vision model of its "
            "endpoint"),
        "Image reader",
        "Describes attached images when the conversation model cannot see "
        "them"),
    registry::Named(
        registry::Int("attachments.maxMb", "UAGENT_ATTACHMENT_MB", {}, 10, 1,
                      kConfigMaxMegabytes, ReloadPolicy::kNextUserTurn, "media",
                      "largest attachment in mebibytes"),
        "Attachment size limit"),
    // Session and artifact retention.
    registry::Named(
        registry::Int("history.days", "UAGENT_HISTORY_DAYS", {}, 30,
                      kConfigAnyMin, kConfigAnyMax, ReloadPolicy::kNextUserTurn,
                      "retention", "days of saved sessions kept"),
        "History kept"),

    // Behaviour switches.
    registry::PerConversation(registry::Named(
        registry::Choice(
            registry::Str("approval.mode", "UAGENT_APPROVAL", "approval", "ask",
                          ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                          "behaviour",
                          "ask, auto reviewer, or yolo for ordinary mutations"),
            kApprovalModes),
        "Approval mode")),
    registry::Named(
        registry::Fallback(
            registry::Str("approval.reviewerModel", "UAGENT_PERMISSION_MODEL",
                          "permission_model", "", ReloadPolicy::kNextUserTurn,
                          Sensitivity::kPublic, "behaviour",
                          "[endpoint/]decision model used by auto permissions"),
            "the endpoint's own decision model"),
        "Permission reviewer", "Judges risky actions when approval is Auto"),
    registry::Named(
        registry::Fallback(
            registry::Str("title.model", "UAGENT_TITLE_MODEL", "title_model",
                          "", ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                          "behaviour",
                          "model route that names new sessions, or off"),
            "the conversation model"),
        "Title model",
        "Names new conversations; off keeps the first message as the title"),
    registry::Named(
        registry::Str("tools.capabilities", "UAGENT_TOOL_CAPABILITIES", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "behaviour", "restrict the exposed tool capability set"),
        "Tool capabilities"),
    registry::Named(
        registry::Str(
            "shell.envAllow", "UAGENT_SHELL_ENV_ALLOW", {}, "",
            ReloadPolicy::kNextUserTurn, Sensitivity::kPublic, "behaviour",
            "comma-separated sensitive variables approved shells may inherit"),
        "Shell variables allowed"),
    registry::Terminal(registry::Named(
        registry::Str("debug.log", "UAGENT_DEBUG_LOG", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "behaviour",
                      "write a sensitive reconstructable JSONL trace"),
        "Debug log")),
    registry::Terminal(registry::Named(
        registry::Bul("terminal.plain", "UAGENT_PLAIN", {}, false,
                      ReloadPolicy::kRestartRequired, "behaviour",
                      "screen-reader terminal: labelled lines, no animation or "
                      "cursor control"),
        "Plain output")),
    registry::Named(
        registry::Choice(
            registry::Str("verbosity", "UAGENT_VERBOSITY", {}, "default",
                          ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                          "behaviour",
                          "how much of the agent's work is shown: minimal, "
                          "default or full; display only"),
            kVerbosityLevels),
        "Detail shown"),
};

inline std::span<const ConfigDescriptor> ConfigRegistry() {
  return kConfigRegistry;
}

const ConfigDescriptor* FindConfigDescriptor(std::string_view environment);
// The same by the name the settings file uses.
const ConfigDescriptor* FindConfigKey(std::string_view key);

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

// Whether a setting takes `value`, and why not, naming the setting as
// `called` (its environment name when empty).
bool ValidSettingValue(const ConfigDescriptor& descriptor, std::string& value,
                       std::string& error, std::string_view called = {});

const char* ConfigTypeName(ConfigType type);
const char* ReloadPolicyName(ReloadPolicy policy);
const char* SensitivityName(Sensitivity sensitivity);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_CONFIG_REGISTRY_H_
