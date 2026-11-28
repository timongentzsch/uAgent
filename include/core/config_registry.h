// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_CONFIG_REGISTRY_H_
#define UAGENT_INCLUDE_CORE_CONFIG_REGISTRY_H_
// The authoritative description of every UAGENT_* setting: default, bounds,
// when a change takes effect, and whether the value may be shown.

#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "include/core/limits.h"
#include "include/core/signals.h"

namespace uagent {

inline constexpr int64_t kConfigAnyMin = std::numeric_limits<int64_t>::min();
inline constexpr int64_t kConfigAnyMax = std::numeric_limits<int64_t>::max();
inline constexpr int64_t kConfigMaxMegabytes = static_cast<int64_t>(
    std::numeric_limits<size_t>::max() / (int64_t{1024} * 1024));

enum class ConfigType { kInt, kDouble, kBool, kString };

// Where a committed change becomes visible. Only RuntimeConfig is re-read at a
// user-turn boundary; a setting read through a live getter needs a restart.
enum class ReloadPolicy { kNextUserTurn, kRestartRequired };

enum class Sensitivity { kPublic, kCompositeSecret };

// Bit flags: a setting may be persistable at more than one layer.
enum ConfigScope : unsigned {
  kScopeUser = 1u << 0,
  kScopeProject = 1u << 1,
  // One conversation's own choice, kept with it.
  kScopeConversation = 1u << 2,
};

// Every scope a value can come from, lowest first.
struct ConfigScopeName {
  std::string_view source;
  std::string_view label;
  unsigned persisted;  // its ConfigScope bit, or 0
};
inline constexpr ConfigScopeName kConfigScopes[] = {
    {"user", "All conversations", kScopeUser},
    {"project", "This folder", kScopeProject},
    {"environment", "Environment", 0},
    {"cli", "Command line", 0},
    {"conversation", "This conversation", kScopeConversation},
};
// Where Settings lists a setting: a topic, and a group inside it, in the
// order and under the names shown there and in docs/interfaces/settings.md.
struct ConfigTopic {
  std::string_view id;
  std::string_view label;
};
inline constexpr ConfigTopic kConfigTopics[] = {
    {"models", "Models and providers"},
    {"approvals", "Approvals and safety"},
    {"tools", "Tools and extensions"},
    {"instructions", "Instructions and memory"},
    {"device", "This device"},
    {"host", "Host and devices"},
    {"about", "About and diagnostics"},
};
struct ConfigGroup {
  std::string_view id;
  std::string_view topic;
  std::string_view label;
};
inline constexpr ConfigGroup kConfigGroups[] = {
    {"conversation", "models", "Conversation"},
    {"providers", "models", "Providers"},
    {"other-models", "models", "Models for other jobs"},
    {"requests", "models", "Requests"},
    {"approval", "approvals", "Approval"},
    {"sandbox", "approvals", "Sandbox"},
    {"limits", "approvals", "Limits"},
    {"tools", "tools", "Tools"},
    {"agents", "tools", "Agents"},
    {"web-search", "tools", "Web search"},
    {"mcp", "tools", "MCP"},
    {"skills", "tools", "Skills"},
    {"instructions", "instructions", "Instructions"},
    {"memory", "instructions", "Memory"},
    {"attachments", "instructions", "Attachments"},
    {"history", "instructions", "History"},
    {"view", "device", "View"},
    {"host", "host", "This host"},
    {"notifications", "host", "Notifications"},
    {"updates", "about", "Update checks"},
    {"logs", "about", "Logs"},
};

constexpr const ConfigGroup* FindConfigGroup(std::string_view id) {
  for (const ConfigGroup& group : kConfigGroups) {
    if (group.id == id) return &group;
  }
  return nullptr;
}

// The name people read for a source; anything else is the default.
constexpr std::string_view ConfigScopeLabel(std::string_view source) {
  for (const ConfigScopeName& scope : kConfigScopes) {
    if (scope.source == source) return scope.label;
  }
  return "Default";
}

using ConfigDefault = std::variant<int64_t, double, bool, std::string_view>;

struct ConfigDescriptor {
  // `environment` holds a string literal, so `.data()` is null-terminated.
  constexpr const char* EnvName() const {
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
  // Its kConfigGroups id; the group names the topic.
  std::string_view group;
  std::string_view description;
  // Accepted spellings of a fixed-choice string; empty means free text.
  std::span<const std::string_view> choices = {};
  // What applies while the value is empty: a setting's name or a short phrase.
  std::string_view fallback = {};
  // A person's name for the setting and what it is for, in a sentence.
  std::string_view label = {};
  std::string_view purpose = {};
  bool terminal = false;
  // Rarely changed: Settings lists it under "Show all".
  bool advanced = false;
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
inline constexpr std::string_view kApprovalModes[] = {"ask", "auto", "yolo",
                                                      "plan"};
inline constexpr std::string_view kSandboxLevels[] = {"readonly", "default",
                                                      "relaxed", "off"};
inline constexpr std::string_view kDetailLevels[] = {"minimal", "default",
                                                     "full"};
inline constexpr std::string_view kLogLevels[] = {"off", "events", "full",
                                                  "debug"};

// Sent when a route has no credential; local OpenAI-compatible servers accept
// any bearer value, and "no key configured" checks compare against it.
inline constexpr char kPlaceholderApiKey[] = "sk-noop";

namespace registry {

// `key` is the row's name in the settings file.
consteval ConfigDescriptor Keyed(std::string_view key,
                                 ConfigDescriptor descriptor) {
  descriptor.key = key;
  return descriptor;
}

consteval ConfigDescriptor Int(std::string_view key, std::string_view env,
                               std::string_view field, int64_t value,
                               int64_t minimum, int64_t maximum,
                               ReloadPolicy reload, std::string_view group,
                               std::string_view description,
                               unsigned scopes = kScopeUser | kScopeProject) {
  return Keyed(key, {env, field, ConfigType::kInt, value, minimum, maximum,
                     reload, Sensitivity::kPublic, scopes, group, description});
}

consteval ConfigDescriptor Str(std::string_view key, std::string_view env,
                               std::string_view field, std::string_view value,
                               ReloadPolicy reload, Sensitivity sensitivity,
                               std::string_view group,
                               std::string_view description,
                               unsigned scopes = kScopeUser | kScopeProject) {
  return Keyed(
      key, {env, field, ConfigType::kString, value, kConfigAnyMin,
            kConfigAnyMax, reload, sensitivity, scopes, group, description});
}

consteval ConfigDescriptor Bul(std::string_view key, std::string_view env,
                               std::string_view field, bool value,
                               ReloadPolicy reload, std::string_view group,
                               std::string_view description,
                               unsigned scopes = kScopeUser | kScopeProject) {
  return Keyed(
      key, {env, field, ConfigType::kBool, value, kConfigAnyMin, kConfigAnyMax,
            reload, Sensitivity::kPublic, scopes, group, description});
}

consteval ConfigDescriptor Dbl(std::string_view key, std::string_view env,
                               std::string_view field, double value,
                               ReloadPolicy reload, std::string_view group,
                               std::string_view description) {
  return Keyed(key, {env, field, ConfigType::kDouble, value, kConfigAnyMin,
                     kConfigAnyMax, reload, Sensitivity::kPublic,
                     kScopeUser | kScopeProject, group, description});
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

// Only a terminal process or a flag reads it; the web lists it with a note.
consteval ConfigDescriptor Terminal(ConfigDescriptor descriptor) {
  descriptor.terminal = true;
  return descriptor;
}

consteval ConfigDescriptor Advanced(ConfigDescriptor descriptor) {
  descriptor.advanced = true;
  return descriptor;
}

}  // namespace registry

inline constexpr ConfigDescriptor kConfigRegistry[] = {
    // The web host is per OS user, so its settings never come from a folder.
    registry::Advanced(registry::Terminal(registry::Named(
        registry::Str(
            "web.bind", "UAGENT_WEB_BIND", {}, "127.0.0.1",
            ReloadPolicy::kRestartRequired, Sensitivity::kPublic, "host",
            "web listener address: loopback by default, all interfaces "
            "only when explicitly configured",
            kScopeUser),
        "Web listen address"))),
    registry::Advanced(registry::Terminal(registry::Named(
        registry::Str("web.browserData", "UAGENT_BROWSER_DATA", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "host",
                      "private browser profile and service directory; empty "
                      "disables the shared browser",
                      kScopeUser),
        "Browser data folder"))),
    registry::Advanced(registry::Terminal(registry::Named(
        registry::Str("web.remoteBundle", "UAGENT_REMOTE_BUNDLE", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "host",
                      "directory containing the verified three-target bundle "
                      "for this build",
                      kScopeUser),
        "Remote bundle folder"))),
    registry::Advanced(registry::Terminal(registry::Named(
        registry::Int("web.port", "UAGENT_WEB_PORT", {}, 8080, 1024, 65535,
                      ReloadPolicy::kRestartRequired, "host",
                      "the web host's loopback port", kScopeUser),
        "Web port"))),
    registry::Advanced(registry::Terminal(registry::Named(
        registry::Str(
            "web.origin", "UAGENT_WEB_ORIGIN", {}, "",
            ReloadPolicy::kRestartRequired, Sensitivity::kPublic, "host",
            "exact browser origin via an explicitly configured HTTPS or "
            "tailnet proxy",
            kScopeUser),
        "Web origin"))),
    registry::Advanced(registry::Named(
        registry::Str("web.pushContact", "UAGENT_WEB_PUSH_CONTACT", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "notifications",
                      "VAPID mailto or HTTPS contact; empty disables optional "
                      "native Web Push",
                      kScopeUser),
        "Push contact")),
    registry::Named(
        registry::Bul("web.notificationDetail",
                      "UAGENT_WEB_NOTIFICATION_DETAIL", {}, true,
                      ReloadPolicy::kRestartRequired, "notifications",
                      "background notifications name the conversation and why "
                      "it needs you; off sends a generic notice",
                      kScopeUser),
        "Notification detail",
        "A notification on the lock screen shows the conversation's title and "
        "why it needs you; it is sent through the browser's push service"),
    registry::Named(
        registry::Bul("web.terminal", "UAGENT_WEB_TERMINAL", {}, true,
                      ReloadPolicy::kRestartRequired, "host",
                      "a shell of yours, unsandboxed, for paired devices in "
                      "the web interface",
                      kScopeUser),
        "Terminal",
        "Paired devices can open a shell of yours, as you and unsandboxed"),
    registry::Named(
        registry::Bul("web.updateCheck", "UAGENT_WEB_UPDATE_CHECK", {}, false,
                      ReloadPolicy::kRestartRequired, "updates",
                      "the web host asks GitHub once a day whether a newer "
                      "release exists",
                      kScopeUser),
        "Check for updates daily",
        "This host asks GitHub for the newest release once a day; About "
        "shows what it found"),
    registry::Advanced(registry::Named(
        registry::Int("web.terminalIdleMinutes",
                      "UAGENT_WEB_TERMINAL_IDLE_MINUTES", {}, 60, 1, 10080,
                      ReloadPolicy::kRestartRequired, "host",
                      "minutes a terminal no device shows may stay silent "
                      "before it ends",
                      kScopeUser),
        "Terminal idle minutes",
        "How long a terminal nobody has open may print nothing before it "
        "ends")),
    registry::Advanced(registry::Named(
        registry::Int(
            "web.terminalScrollbackKb", "UAGENT_WEB_TERMINAL_SCROLLBACK_KB", {},
            static_cast<int64_t>(kWebTerminalScrollbackBytes / KiB(1)), 64,
            16384, ReloadPolicy::kRestartRequired, "host",
            "kibibytes of a web terminal's output kept for scrolling "
            "back and for a device that connects later",
            kScopeUser),
        "Terminal scrollback")),
    registry::PerConversation(registry::Named(
        registry::Fallback(
            registry::Str(
                "model", "UAGENT_MODEL",
                {}, "", ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                "conversation",
                "model or named route as [provider/]model[:variant][:effort]"),
            "the provider default"),
        "Model",
        "The model new conversations start with; each can choose its own "
        "in the input")),
    registry::Named(
        registry::Fallback(
            registry::Choice(
                registry::Str(
                    "reasoningEffort", "UAGENT_REASONING_EFFORT", {}, "",
                    ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                    "conversation",
                    "none, minimal, low, medium, high, xhigh, or max"),
                kReasoningEfforts),
            "provider default"),
        "Reasoning effort",
        "How long the conversation model thinks before answering"),
    registry::Named(
        registry::Str(
            "providers", "UAGENT_PROVIDERS", {}, "",
            ReloadPolicy::kRestartRequired, Sensitivity::kCompositeSecret,
            "providers",
            "JSON object of named endpoints, transports, and aliases"),
        "Providers",
        "Addresses that serve models, each with its key; a model is chosen as "
        "provider/model"),
    registry::Named(
        registry::Int("contextWindow", "UAGENT_CONTEXT", {}, 0, kConfigAnyMin,
                      kConfigAnyMax, ReloadPolicy::kRestartRequired,
                      "conversation",
                      "context-window tokens; 0 uses the provider profile"),
        "Context window"),
    registry::Advanced(registry::Named(
        registry::Int("maxOutputTokens", "UAGENT_MAX_TOKENS", {}, -1,
                      kConfigAnyMin, kConfigAnyMax, ReloadPolicy::kNextUserTurn,
                      "conversation",
                      "maximum response tokens; -1 omits the optional cap"),
        "Response length")),

    registry::Advanced(registry::Named(
        registry::Int(
            "request.streamTimeout", "UAGENT_STREAM_TIMEOUT",
            "stream_timeout_s", 300, kConfigAnyMin, kConfigAnyMax,
            ReloadPolicy::kNextUserTurn, "requests",
            "seconds of stream silence allowed, before the first event "
            "or between events"),
        "Stream timeout")),
    registry::Advanced(registry::Named(
        registry::Int("request.timeout", "UAGENT_REQUEST_TIMEOUT",
                      "request_timeout_s", 600, kConfigAnyMin, kConfigAnyMax,
                      ReloadPolicy::kNextUserTurn, "requests",
                      "total seconds allowed for one model request"),
        "Request timeout")),
    registry::Advanced(registry::Named(
        registry::Int("request.connectSeconds", "UAGENT_CONNECT_SECONDS", {},
                      kConnectSeconds, 1, 300, ReloadPolicy::kNextUserTurn,
                      "requests",
                      "seconds a request waits for the server to accept the "
                      "connection",
                      kScopeUser),
        "Connect timeout")),
    registry::Advanced(registry::Named(
        registry::Int("request.probeSeconds", "UAGENT_PROBE_SECONDS", {},
                      kProbeSeconds, 1, 120, ReloadPolicy::kNextUserTurn,
                      "requests",
                      "seconds a quick check may take: a key, a model list, "
                      "the update check",
                      kScopeUser),
        "Quick-check timeout")),

    registry::Advanced(registry::Named(
        registry::Int("limits.maxSteps", "UAGENT_MAX_STEPS", "max_steps", 0, 0,
                      kConfigAnyMax, ReloadPolicy::kNextUserTurn, "limits",
                      "model rounds per turn; 0 disables the limit"),
        "Steps per turn")),
    registry::Advanced(registry::Named(
        registry::Int("limits.maxToolCalls", "UAGENT_MAX_TOOL_CALLS",
                      "max_tool_calls", 0, 0, kConfigAnyMax,
                      ReloadPolicy::kNextUserTurn, "limits",
                      "tool calls per turn; 0 disables the limit"),
        "Tool calls per turn")),
    registry::Advanced(registry::Named(
        registry::Int("limits.repeatedCallStop", "UAGENT_REPEATED_CALL_STOP",
                      {}, 12, 7, 1000, ReloadPolicy::kNextUserTurn, "limits",
                      "identical consecutive tool calls that end the turn; "
                      "the model is warned well before it"),
        "Repeated call limit")),
    registry::Advanced(registry::Named(
        registry::Int("limits.maxTurnSeconds", "UAGENT_MAX_TURN_SECONDS",
                      "max_turn_seconds", 0, 0, kConfigAnyMax,
                      ReloadPolicy::kNextUserTurn, "limits",
                      "wall-clock seconds per turn; 0 disables the deadline"),
        "Turn time limit")),
    registry::Advanced(registry::Named(
        registry::Int("limits.maxTurnTokens", "UAGENT_MAX_TURN_TOKENS",
                      "max_turn_tokens", 0, 0, kConfigAnyMax,
                      ReloadPolicy::kNextUserTurn, "limits",
                      "generated-token ceiling per turn; 0 disables it"),
        "Tokens per turn")),
    registry::Advanced(registry::Named(
        registry::Int("limits.sessionTokens", "UAGENT_SESSION_TOKEN_BUDGET",
                      "session_token_budget", 0, 0, kConfigAnyMax,
                      ReloadPolicy::kNextUserTurn, "limits",
                      "cumulative generated-token ceiling; 0 disables it"),
        "Conversation token budget")),
    registry::Named(
        registry::Dbl("limits.maxTurnCost", "UAGENT_MAX_TURN_COST",
                      "max_turn_cost", 0.0, ReloadPolicy::kNextUserTurn,
                      "limits",
                      "reported-cost ceiling per turn; 0 disables it"),
        "Cost per turn"),
    registry::Named(
        registry::Dbl("limits.sessionCost", "UAGENT_SESSION_BUDGET",
                      "session_budget", 0.0, ReloadPolicy::kNextUserTurn,
                      "limits",
                      "cumulative reported-cost ceiling; 0 disables it"),
        "Conversation budget"),
    registry::Advanced(registry::Named(
        registry::Int("tools.timeout", "UAGENT_TOOL_TIMEOUT", "tool_timeout_s",
                      30, 0, kConfigAnyMax, ReloadPolicy::kNextUserTurn,
                      "limits", "seconds one tool call may run"),
        "Tool timeout")),
    registry::Named(
        registry::Int("context.autoCompactPercent", "UAGENT_AUTO_COMPACT_PCT",
                      {}, 85, kConfigAnyMin, kConfigAnyMax,
                      ReloadPolicy::kNextUserTurn, "conversation",
                      "context percentage that triggers compaction"),
        "Compact at"),
    registry::Advanced(registry::Named(
        registry::Int(
            "context.compactionInputKb", "UAGENT_COMPACTION_INPUT_KB", {},
            static_cast<int64_t>(kCompactionTranscriptBytes / KiB(1)),
            static_cast<int64_t>(kCompactionTranscriptFloorBytes / KiB(1)),
            4096, ReloadPolicy::kNextUserTurn, "conversation",
            "kibibytes of the conversation the summary model reads "
            "when the context is compacted",
            kScopeUser),
        "Compaction input")),

    registry::Advanced(registry::Named(
        registry::Int("tools.resultChars", "UAGENT_TOOL_RESULT_CHARS", {}, 8000,
                      kConfigAnyMin, kConfigAnyMax, ReloadPolicy::kNextUserTurn,
                      "tools", "characters kept from one tool result"),
        "Tool result size")),
    registry::Advanced(registry::Named(
        registry::Int("tools.readFileLines", "UAGENT_READ_FILE_LINES", {}, 1000,
                      kConfigAnyMin, kConfigAnyMax, ReloadPolicy::kNextUserTurn,
                      "tools", "default lines returned by read_path"),
        "Lines per file read")),
    registry::Advanced(registry::Named(
        registry::Int("tools.readFileKb", "UAGENT_READ_FILE_KB", {}, 32, 1,
                      4096, ReloadPolicy::kRestartRequired, "tools",
                      "kibibytes one read_path call returns"),
        "File read size")),
    registry::Advanced(registry::Named(
        registry::Int("tools.editFileMb", "UAGENT_EDIT_FILE_MB", {}, 10, 1,
                      1024, ReloadPolicy::kNextUserTurn, "tools",
                      "largest file in mebibytes the file tools edit or "
                      "diff"),
        "Editable file size")),
    registry::Advanced(registry::Named(
        registry::Int("tools.listEntries", "UAGENT_LIST_ENTRIES", {}, 1000, 1,
                      100000, ReloadPolicy::kNextUserTurn, "tools",
                      "entries one folder listing returns"),
        "Folder entries listed")),
    registry::Advanced(registry::Named(
        registry::Int("tools.grepResults", "UAGENT_GREP_RESULTS", {}, 200, 1,
                      10000, ReloadPolicy::kNextUserTurn, "tools",
                      "matching lines one search returns"),
        "Search results")),
    registry::Advanced(registry::Named(
        registry::Int("tools.concurrency", "UAGENT_TOOL_CONCURRENCY", {}, 4, 1,
                      32, ReloadPolicy::kNextUserTurn, "tools",
                      "tool calls of one batch that run side by side"),
        "Parallel tool calls")),
    registry::Advanced(registry::Named(
        registry::Int("activities.maxConcurrent", "UAGENT_MAX_ACTIVITIES", {},
                      8, 1, 64, ReloadPolicy::kNextUserTurn, "tools",
                      "commands one conversation runs at once"),
        "Activities at once")),
    registry::Advanced(registry::Named(
        registry::Int("activities.logMb", "UAGENT_ACTIVITY_LOG_MB", {}, 64, 1,
                      4096, ReloadPolicy::kNextUserTurn, "tools",
                      "largest log in mebibytes kept per command"),
        "Command log size")),

    // Restart-required: the sandbox policy is built once and wraps every spawn.
    registry::Named(
        registry::Choice(
            registry::Str("sandbox.level", "UAGENT_SANDBOX", {}, "default",
                          ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                          "sandbox",
                          "how far the OS sandbox confines shell commands: "
                          "readonly (temporary files only, no network), "
                          "default (relaxed "
                          "under yolo), relaxed (home writable and "
                          "readable) or "
                          "off; below relaxed, commands read in your home "
                          "folder only this folder and their tools' folders"),
            kSandboxLevels),
        "Sandbox"),
    registry::Named(
        registry::Bul("sandbox.network", "UAGENT_SANDBOX_NET", {}, true,
                      ReloadPolicy::kRestartRequired, "sandbox",
                      "let sandboxed commands reach the network"),
        "Sandbox network access"),
    registry::Named(
        registry::Str("sandbox.write", "UAGENT_SANDBOX_WRITE", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "sandbox",
                      "extra writable roots for the sandbox, colon-separated"),
        "Sandbox writable folders"),
    registry::Named(
        registry::Str("sandbox.read", "UAGENT_SANDBOX_READ", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "sandbox",
                      "folders in your home folder that commands and file "
                      "tools may read without asking, colon-separated"),
        "Sandbox readable folders"),
    registry::Advanced(registry::Named(
        registry::Str("sandbox.hide", "UAGENT_SANDBOX_HIDE", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "sandbox",
                      "more paths the sandbox hides from commands and file "
                      "tools, colon-separated"),
        "Sandbox hidden paths")),
    registry::Advanced(registry::Named(
        registry::Str("sandbox.hideEnv", "UAGENT_SANDBOX_HIDE_ENV", {}, "",
                      ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                      "sandbox",
                      "more environment variables withheld from commands, MCP "
                      "servers and delegated agents, comma-separated"),
        "Hidden variables")),
    registry::Advanced(registry::Named(
        registry::Str("sandbox.reveal", "UAGENT_SANDBOX_REVEAL", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "sandbox",
                      "paths hidden by default that commands and file tools "
                      "may read, colon-separated"),
        "Sandbox revealed paths")),

    registry::Named(
        registry::Int("agents.max", "UAGENT_AGENTS_MAX", {}, 8, 1, 64,
                      ReloadPolicy::kNextUserTurn, "agents",
                      "agents one conversation may have at work at once"),
        "Agents at once"),
    registry::Advanced(registry::Named(
        registry::Int("agents.depth", "UAGENT_AGENTS_DEPTH", {}, 2, 0,
                      kConfigAnyMax, ReloadPolicy::kRestartRequired, "agents",
                      "how deep agents may start agents of their own"),
        "Agent depth")),
    registry::Advanced(registry::Named(
        registry::Int("agents.parentDecisionSeconds",
                      "UAGENT_PARENT_DECISION_SECONDS", {},
                      kParentDecisionSeconds, 1, 86400,
                      ReloadPolicy::kNextUserTurn, "agents",
                      "seconds an agent waits for its parent to decide on an "
                      "approval before asking the person",
                      kScopeUser),
        "Parent decision wait")),
    registry::Advanced(registry::Named(
        registry::Int("agents.reportChars", "UAGENT_AGENT_REPORT_CHARS", {},
                      kAgentReportChars, 500, 48000,
                      ReloadPolicy::kNextUserTurn, "agents",
                      "characters of an agent's final answer its parent reads",
                      kScopeUser),
        "Agent report length")),

    registry::Named(
        registry::Choice(
            registry::Str("webSearch.backend", "UAGENT_WEB_SEARCH_BACKEND",
                          "web_search_backend", "auto",
                          ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                          "web-search", "auto or off"),
            kWebSearchBackends),
        "Web search"),
    registry::Named(
        registry::Fallback(
            registry::Str("webSearch.model", "UAGENT_WEB_SEARCH_MODEL",
                          "web_search_model", "", ReloadPolicy::kNextUserTurn,
                          Sensitivity::kPublic, "web-search",
                          "model route used for search"),
            "the conversation model, on an endpoint that offers search"),
        "Web search model", "Answers web lookups"),
    registry::Advanced(registry::Named(
        registry::Int("webSearch.maxResults", "UAGENT_WEB_SEARCH_RESULTS", {},
                      5, 1, 50, ReloadPolicy::kRestartRequired, "web-search",
                      "results one search query returns"),
        "Results per search")),
    registry::Advanced(registry::Named(
        registry::Int("webSearch.maxTokens", "UAGENT_WEB_SEARCH_MAX_TOKENS", {},
                      kWebSearchMaxTokens, 500, 64000,
                      ReloadPolicy::kNextUserTurn, "web-search",
                      "tokens the web-search model's answer may take",
                      kScopeUser),
        "Search answer length")),
    registry::Advanced(registry::Named(
        registry::Int("webSearch.maxUses", "UAGENT_WEB_SEARCH_MAX_USES", {}, 3,
                      1, 10, ReloadPolicy::kRestartRequired, "web-search",
                      "queries one web_search call may carry"),
        "Queries per search")),
    registry::Advanced(registry::Named(
        registry::Int("webSearch.callsPerTurn", "UAGENT_WEB_SEARCH_CALLS", {},
                      4, 1, 100, ReloadPolicy::kRestartRequired, "web-search",
                      "web_search calls one turn may make"),
        "Searches per turn")),
    registry::Advanced(registry::Named(
        registry::Int("webSearch.timeout", "UAGENT_WEB_SEARCH_TIMEOUT", {}, 60,
                      1, 600, ReloadPolicy::kRestartRequired, "web-search",
                      "seconds one search request may take"),
        "Search timeout")),

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
    registry::Advanced(
        registry::Named(registry::Int("memory.max", "UAGENT_MEMORY_MAX", {}, 32,
                                      1, 1000, ReloadPolicy::kNextUserTurn,
                                      "memory", "memories kept per scope"),
                        "Memories per scope")),
    registry::Advanced(registry::Named(
        registry::Int("memory.maxBytes", "UAGENT_MEMORY_BYTES", {}, 2048, 256,
                      65536, ReloadPolicy::kNextUserTurn, "memory",
                      "bytes one memory may hold"),
        "Memory size")),
    registry::Advanced(registry::Named(
        registry::Int("memory.alwaysBytes", "UAGENT_MEMORY_ALWAYS_BYTES", {},
                      2048, 0, 65536, ReloadPolicy::kRestartRequired, "memory",
                      "bytes of global memories placed whole in every "
                      "conversation's context"),
        "Always-on memory")),
    registry::Advanced(registry::Named(
        registry::Int("memory.timeoutSeconds", "UAGENT_MEMORY_SECONDS", {},
                      kMemoryExtractSeconds, 30, 3600,
                      ReloadPolicy::kNextUserTurn, "memory",
                      "seconds the memory extractor may take for one "
                      "conversation",
                      kScopeUser),
        "Memory extraction timeout")),
    registry::Advanced(registry::Named(
        registry::Int("memory.idleHours", "UAGENT_MEMORY_IDLE_HOURS", {}, 6, 1,
                      720, ReloadPolicy::kNextUserTurn, "memory",
                      "hours a saved conversation sits idle before memories "
                      "are extracted from it"),
        "Memory extraction delay")),

    registry::Advanced(registry::Terminal(registry::Named(
        registry::Str("skills.path", "UAGENT_SKILL_PATH", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "skills", "replace the entire skill search path"),
        "Skill folders"))),
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
    registry::Advanced(registry::Named(
        registry::Int("skills.max", "UAGENT_SKILLS_MAX", {}, 64, 1, 1000,
                      ReloadPolicy::kRestartRequired, "skills",
                      "skills offered at once; later ones replace earlier"),
        "Skills offered")),
    registry::Advanced(registry::Named(
        registry::Int("instructions.maxKb", "UAGENT_INSTRUCTIONS_KB", {}, 32, 1,
                      1024, ReloadPolicy::kRestartRequired, "instructions",
                      "kibibytes of folder instructions loaded or written"),
        "Instructions size")),

    registry::Advanced(registry::Named(
        registry::Int(
            "mcp.timeout", "UAGENT_MCP_TIMEOUT", "mcp_timeout_s", 60, 1,
            kConfigAnyMax, ReloadPolicy::kRestartRequired, "mcp",
            "seconds an MCP server has for each request, except the retry "
            "after an accepted link"),
        "MCP call timeout")),
    registry::Advanced(registry::Named(
        registry::Int("mcp.maxServers", "UAGENT_MCP_MAX_SERVERS", {}, 32, 1,
                      256, ReloadPolicy::kRestartRequired, "mcp",
                      "MCP servers started; the rest are listed as skipped"),
        "MCP servers")),
    registry::Advanced(registry::Named(
        registry::Int("mcp.maxTools", "UAGENT_MCP_MAX_TOOLS", {}, 256, 1, 4096,
                      ReloadPolicy::kRestartRequired, "mcp",
                      "tools one MCP server may list"),
        "MCP tools per server")),

    registry::Named(
        registry::Fallback(
            registry::Str(
                "image.model", "UAGENT_IMAGE_MODEL",
                "image_model", "", ReloadPolicy::kNextUserTurn,
                Sensitivity::kPublic, "other-models",
                "model route that reads attached images; empty uses the "
                "main route when it reads images, else the first model on "
                "its endpoint that does"),
            "main route if it reads images, else a vision model of its "
            "endpoint"),
        "Image reader",
        "Describes attached images when the conversation model cannot see "
        "them"),
    registry::Advanced(registry::Named(
        registry::Int("attachments.maxMb", "UAGENT_ATTACHMENT_MB", {}, 10, 1,
                      kConfigMaxMegabytes, ReloadPolicy::kNextUserTurn,
                      "attachments",
                      "largest attachment or web upload in mebibytes; the web "
                      "host takes a new value when it restarts"),
        "Attachment size limit")),
    registry::Advanced(registry::Named(
        registry::Int("attachments.maxCount", "UAGENT_ATTACHMENT_COUNT", {}, 8,
                      1, 64, ReloadPolicy::kNextUserTurn, "attachments",
                      "attachments queued for one model request"),
        "Attachments per request")),
    registry::Advanced(registry::Named(
        registry::Int("attachments.storeMb", "UAGENT_ATTACHMENT_STORE_MB", {},
                      kAttachmentStoreMegabytes, 64, 65536,
                      ReloadPolicy::kNextUserTurn, "attachments",
                      "mebibytes kept for files uploaded in the web app, "
                      "across conversations; a full store refuses new ones",
                      kScopeUser),
        "Upload storage")),
    registry::Advanced(registry::Named(
        registry::Int("attachments.maxImagePx", "UAGENT_IMAGE_MAX_PX", {}, 2048,
                      256, 8192, ReloadPolicy::kNextUserTurn, "attachments",
                      "longest image side in pixels; larger images are "
                      "scaled down"),
        "Image size")),
    registry::Named(
        registry::Int("history.days", "UAGENT_HISTORY_DAYS", {}, 30,
                      kConfigAnyMin, kConfigAnyMax, ReloadPolicy::kNextUserTurn,
                      "history", "days of saved sessions kept"),
        "History kept"),
    registry::Advanced(registry::Named(
        registry::Int("history.files", "UAGENT_HISTORY_FILES", {}, 200, 1,
                      100000, ReloadPolicy::kNextUserTurn, "history",
                      "most saved sessions kept"),
        "Sessions kept")),
    registry::Named(
        registry::Int("logs.keepDays", "UAGENT_LOG_DAYS", {}, 14, 1, 3650,
                      ReloadPolicy::kNextUserTurn, "logs",
                      "days of session, activity, mail and MCP logs kept"),
        "Logs kept"),
    registry::Named(
        registry::Int("logs.keepMb", "UAGENT_LOG_MB", {}, 1024, 1, 1048576,
                      ReloadPolicy::kNextUserTurn, "logs",
                      "mebibytes of logs kept; the oldest go first"),
        "Log space"),
    registry::Named(
        registry::Choice(
            registry::Str("logs.level", "UAGENT_LOG_LEVEL", {}, "full",
                          ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                          "logs",
                          "what the agent records of its work: off, events "
                          "(journal, MCP stderr), full (also requests and "
                          "tool exchanges) or debug (also the trace)"),
            kLogLevels),
        "Log level"),

    registry::PerConversation(registry::Named(
        registry::Choice(
            registry::Str("approval.mode", "UAGENT_APPROVAL", "approval", "ask",
                          ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                          "approval",
                          "ask, auto reviewer, or yolo for ordinary mutations; "
                          "plan only reads until a plan is approved"),
            kApprovalModes),
        "Approval",
        "The approval new conversations start with; each can choose its own "
        "in the input")),
    registry::Advanced(registry::Named(
        registry::Int("runtime.idleMinutes", "UAGENT_IDLE_MINUTES", {}, 15, 1,
                      1440, ReloadPolicy::kNextUserTurn, "host",
                      "minutes with nothing to do before a conversation's "
                      "runtime and Chrome stop; the next message starts "
                      "them again"),
        "Idle timeout")),
    registry::Named(
        registry::Fallback(
            registry::Str("approval.reviewerModel", "UAGENT_PERMISSION_MODEL",
                          "permission_model", "", ReloadPolicy::kNextUserTurn,
                          Sensitivity::kPublic, "approval",
                          "[provider/]decision model of Auto; a provider other "
                          "than the conversation's reviews only when named"),
            "the endpoint's own decision model"),
        "Permission reviewer", "Judges risky actions when approval is Auto"),
    registry::Named(
        registry::Fallback(
            registry::Str("title.model", "UAGENT_TITLE_MODEL", "title_model",
                          "", ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                          "other-models",
                          "model route that names new sessions, or off"),
            "the conversation model"),
        "Title model",
        "Names new conversations; off keeps the first message as the title"),
    registry::Advanced(registry::Named(
        registry::Str("tools.capabilities", "UAGENT_TOOL_CAPABILITIES", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "approval", "restrict the exposed tool capability set"),
        "Tool capabilities")),
    registry::Advanced(registry::Named(
        registry::Int("title.timeoutSeconds", "UAGENT_TITLE_SECONDS", {},
                      kTitleTimeoutSeconds, 5, 600, ReloadPolicy::kNextUserTurn,
                      "other-models",
                      "seconds the call that names a conversation may take",
                      kScopeUser),
        "Title timeout")),
    registry::Advanced(registry::Named(
        registry::Str(
            "shell.envAllow", "UAGENT_SHELL_ENV_ALLOW", {}, "",
            ReloadPolicy::kNextUserTurn, Sensitivity::kPublic, "sandbox",
            "comma-separated sensitive variables approved shells may inherit"),
        "Shell variables allowed")),
    registry::Advanced(registry::Terminal(registry::Named(
        registry::Bul("terminal.plain", "UAGENT_PLAIN", {}, false,
                      ReloadPolicy::kRestartRequired, "view",
                      "screen-reader terminal: labelled lines, no animation or "
                      "cursor control"),
        "Plain output"))),
    registry::Named(
        registry::Choice(
            registry::Str("detail", "UAGENT_DETAIL", {}, "minimal",
                          ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                          "view",
                          "how much of the agent's work is shown: minimal, "
                          "default or full; display only"),
            kDetailLevels),
        "View",
        "How much of the agent's work a device shows until it picks its own "
        "view"),
    registry::Named(
        registry::Bul("thinking", "UAGENT_THINKING", {}, false,
                      ReloadPolicy::kNextUserTurn, "view",
                      "show the model's thinking in the transcript; display "
                      "only"),
        "Thinking",
        "Whether a device shows the model's thinking until it picks its own "
        "view"),
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

// Compile-time descriptor lookup.
consteval const ConfigDescriptor& Cfg(std::string_view environment) {
  for (const ConfigDescriptor& descriptor : kConfigRegistry) {
    if (descriptor.environment == environment) return descriptor;
  }
  UnregisteredConfigSetting();
  return kConfigRegistry[0];
}

// A session's settings; environ is never rewritten after startup. Lookup
// order: override, published configuration, the route in use, environ.
using SettingValues = std::map<std::string, std::string>;
void PublishSettings(SettingValues values);
// The route in use, read where the configuration names nothing.
void PublishRoute(SettingValues values);
void OverrideSetting(std::string_view environment, std::string value);
void ClearSettings();
SettingValues CurrentSettings();
std::string SettingText(const ConfigDescriptor& descriptor);
// The same by name, for names that are no registered setting.
std::string SettingText(const std::string& name);

int64_t LongSetting(const ConfigDescriptor& descriptor);
bool BoolSetting(const ConfigDescriptor& descriptor);
std::string StringSetting(const ConfigDescriptor& descriptor);

// Whether a setting takes `value`; `error` names it as `called` when given.
bool ValidSettingValue(const ConfigDescriptor& descriptor, std::string& value,
                       std::string& error, std::string_view called = {});

const char* ConfigTypeName(ConfigType type);
const char* ReloadPolicyName(ReloadPolicy policy);
const char* SensitivityName(Sensitivity sensitivity);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_CONFIG_REGISTRY_H_
