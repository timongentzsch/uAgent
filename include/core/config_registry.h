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
};

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
inline constexpr std::string_view kWebSearchBackends[] = {"auto", "openrouter",
                                                          "off"};
inline constexpr std::string_view kApprovalModes[] = {"ask", "auto", "yolo"};
inline constexpr std::string_view kThreadEnvironments[] = {"worktree",
                                                           "local"};
inline constexpr std::string_view kWebSearchEngines[] = {
    "auto", "native", "exa", "firecrawl", "parallel", "perplexity"};
inline constexpr std::string_view kWebSearchContextSizes[] = {"low", "medium",
                                                              "high"};

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

}  // namespace registry

inline constexpr ConfigDescriptor kConfigRegistry[] = {
    // The web master is per OS user, so its settings never come from a project.
    registry::Str("UAGENT_WEB_BIND", {}, "127.0.0.1",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic, "web",
                  "web listener address: loopback by default, all interfaces "
                  "only when explicitly configured",
                  kScopeUser),
    registry::Str("UAGENT_BROWSER_DATA", {}, "", ReloadPolicy::kRestartRequired,
                  Sensitivity::kPublic, "web",
                  "private browser profile and service directory; empty "
                  "disables the browser appliance",
                  kScopeUser),
    registry::Int("UAGENT_WEB_PORT", {}, 8080, 1024, 65535,
                  ReloadPolicy::kRestartRequired, "web",
                  "global web master's loopback port", kScopeUser),
    registry::Str("UAGENT_WEB_ORIGIN", {}, "", ReloadPolicy::kRestartRequired,
                  Sensitivity::kPublic, "web",
                  "exact browser origin via an explicitly configured HTTPS or "
                  "tailnet proxy",
                  kScopeUser),
    registry::Str("UAGENT_WEB_PUSH_CONTACT", {}, "",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic, "web",
                  "VAPID mailto or HTTPS contact; empty disables optional "
                  "native Web Push",
                  kScopeUser),
    // Route selection and credentials.
    registry::Fallback(
        registry::Str("UAGENT_BASE_URL", {}, "", ReloadPolicy::kRestartRequired,
                      Sensitivity::kPublic, "route", "active API base URL"),
        "OpenRouter when OPENROUTER_API_KEY is set"),
    registry::Str("UAGENT_API_KEY", {}, kPlaceholderApiKey,
                  ReloadPolicy::kRestartRequired, Sensitivity::kSecret, "route",
                  "credential for the active route"),
    registry::Str("OPENROUTER_API_KEY", {}, "", ReloadPolicy::kRestartRequired,
                  Sensitivity::kSecret, "route",
                  "OpenRouter credential used when no base URL is set"),
    registry::Fallback(
        registry::Str(
            "UAGENT_MODEL", {}, "", ReloadPolicy::kRestartRequired,
            Sensitivity::kPublic, "route",
            "model or named route as [provider/]model[:variant][:effort]"),
        "last /model choice, else the provider default"),
    registry::Fallback(
        registry::Str("UAGENT_REASONING_EFFORT", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "route",
                      "none, minimal, low, medium, high, xhigh, or max"),
        "provider default"),
    registry::Str("UAGENT_PROVIDERS", {}, "", ReloadPolicy::kRestartRequired,
                  Sensitivity::kCompositeSecret, "route",
                  "JSON object of named endpoints, transports, and aliases"),
    registry::Str("UAGENT_WIRE_API", {}, "chat_completions",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic, "route",
                  "chat_completions, responses, or anthropic_messages"),
    registry::Str("UAGENT_HOSTED_TOOLS", {}, "", ReloadPolicy::kRestartRequired,
                  Sensitivity::kPublic, "route",
                  "comma-separated hosted capabilities; currently web_search"),
    registry::Str(
        "UAGENT_MODEL_FEATURES", {}, "", ReloadPolicy::kRestartRequired,
        Sensitivity::kPublic, "route",
        "JSON model capabilities: reasoning_summary, adaptive_thinking"),
    registry::Str("UAGENT_PROVIDER_PROTOCOL", {}, "",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic, "route",
                  "openai or openrouter (anthropic is an alias of openai)"),
    registry::Str("UAGENT_OPENROUTER_PROVIDER", "openrouter_provider", "",
                  ReloadPolicy::kNextUserTurn, Sensitivity::kPublic, "route",
                  "pin OpenRouter to one upstream provider"),
    registry::Choice(
        registry::Str("UAGENT_OPENROUTER_VARIANT", "openrouter_variant", "",
                      ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                      "route", "nitro, floor, or exacto routing preference"),
        kOpenRouterVariants),
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
    registry::Int("UAGENT_SESSION_TOKEN_BUDGET", "session_token_budget", 0, 0,
                  kConfigAnyMax, ReloadPolicy::kNextUserTurn, "budget",
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
    registry::Bul("UAGENT_PRUNE_SUPERSEDED_READS", {}, false,
                  ReloadPolicy::kRestartRequired, "tools",
                  "experimental step-boundary pruning of superseded reads"),
    registry::Int("UAGENT_TOOL_RESULT_CHARS", {}, 8000, kConfigAnyMin,
                  kConfigAnyMax, ReloadPolicy::kRestartRequired, "tools",
                  "characters kept from one tool result"),
    registry::Int("UAGENT_READ_FILE_LINES", {}, 1000, kConfigAnyMin,
                  kConfigAnyMax, ReloadPolicy::kRestartRequired, "tools",
                  "default lines returned by read_path"),
    registry::Int("UAGENT_MAX_BACKGROUND_JOBS", {}, 8, 1, kBgMax,
                  ReloadPolicy::kRestartRequired, "tools",
                  "concurrent detached activities"),

    // OS sandbox for agent-run commands. Restart-required because the policy is
    // built once and every spawn is wrapped with it; a mid-session change would
    // leave already-running jobs under the old confinement.
    registry::Bul("UAGENT_SANDBOX", {}, true, ReloadPolicy::kRestartRequired,
                  "tools", "confine shell commands with the OS sandbox"),
    registry::Bul("UAGENT_SANDBOX_NET", {}, true,
                  ReloadPolicy::kRestartRequired, "tools",
                  "let sandboxed commands reach the network"),
    registry::Str("UAGENT_SANDBOX_WRITE", {}, "",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic, "tools",
                  "extra writable roots for the sandbox, colon-separated"),

    // Delegation.
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
    registry::Fallback(
        registry::Str("UAGENT_SUBAGENT_MODEL", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "delegation",
                      "default model route for delegated children"),
        "UAGENT_MODEL"),
    registry::Str("UAGENT_TOOLSET", {}, "", ReloadPolicy::kRestartRequired,
                  Sensitivity::kPublic, "delegation",
                  "lean withholds implementation tools from this process"),

    // Coordination.
    registry::Fallback(
        registry::Str("UAGENT_COORDINATOR_MODEL", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "coordination",
                      "model route of each folder's coordinator; /model "
                      "inside it overrides this for that folder"),
        "UAGENT_MODEL"),
    registry::Int("UAGENT_COORDINATOR_MAX_THREADS", {}, 5, 1, 64,
                  ReloadPolicy::kRestartRequired, "coordination",
                  "threads one coordinator may run at once"),
    registry::Dbl("UAGENT_COORDINATOR_DAILY_SPEND_USD", {}, 20.0,
                  ReloadPolicy::kRestartRequired, "coordination",
                  "reported cost a coordinator's threads may spend per day; "
                  "0 disables it"),
    registry::Choice(
        registry::Str("UAGENT_COORDINATOR_ENVIRONMENT", {}, "worktree",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "coordination",
                      "where threads run: a fresh git worktree, or the "
                      "folder itself"),
        kThreadEnvironments),

    // Web search.
    registry::Choice(
        registry::Str("UAGENT_WEB_SEARCH_BACKEND", "web_search_backend", "auto",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "search", "auto, openrouter, or off"),
        kWebSearchBackends),
    registry::Fallback(
        registry::Str("UAGENT_WEB_SEARCH_MODEL", "web_search_model", "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "search", "model route used for search"),
        "conversation model on OpenRouter, else the default route"),
    registry::Fallback(
        registry::Str("UAGENT_WEB_SEARCH_EFFORT", "web_search_effort", "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "search", "reasoning effort for the search route"),
        "provider default"),
    registry::Choice(
        registry::Str("UAGENT_WEB_SEARCH_ENGINE", "web_search_engine", "auto",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "search",
                      "auto, native, exa, firecrawl, parallel, perplexity"),
        kWebSearchEngines),
    registry::Choice(
        registry::Str("UAGENT_WEB_SEARCH_CONTEXT_SIZE",
                      "web_search_context_size", "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "search", "low, medium, or high native search context"),
        kWebSearchContextSizes),

    // Memory.
    registry::Bul("UAGENT_MEMORY", "memory_enabled", true,
                  ReloadPolicy::kRestartRequired, "memory",
                  "enable memory recall and writes"),
    registry::Bul("UAGENT_MEMORY_GENERATE", "memory_generate", true,
                  ReloadPolicy::kRestartRequired, "memory",
                  "run the background memory extractor"),
    registry::Int("UAGENT_MEMORY_IDLE_SECONDS", {}, int64_t{6} * 60 * 60, 0,
                  int64_t{48} * 60 * 60, ReloadPolicy::kRestartRequired,
                  "memory", "idle seconds before background extraction runs"),
    registry::Fallback(
        registry::Str("UAGENT_MEMORY_MODEL", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "memory", "model route for background memory extraction"),
        "UAGENT_MODEL"),

    // Skills.
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
    registry::Str("UAGENT_MCP_ROOTS", "mcp_roots", "",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic, "mcp",
                  "roots advertised to MCP servers"),

    // Attachments and terminal media.
    registry::Fallback(
        registry::Str("UAGENT_IMAGE_MODEL", "image_model", "",
                      ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                      "media",
                      "model route that reads attached images; empty uses the "
                      "main route when it reads images, else the shared "
                      "default route"),
        "main route if it reads images, else the default route"),
    registry::Fallback(
        registry::Str("UAGENT_IMAGE_DETAIL", {}, "",
                      ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                      "media", "low, high, or original image detail"),
        "provider default"),
    registry::Str("UAGENT_PDF_ENGINE", "pdf_engine", "cloudflare-ai",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic, "media",
                  "OpenRouter file-parser engine for documents"),
    registry::Int("UAGENT_ATTACHMENT_MB", {}, 10, 1, kConfigMaxMegabytes,
                  ReloadPolicy::kRestartRequired, "media",
                  "largest attachment in mebibytes"),
    // Session and artifact retention.
    registry::Int("UAGENT_HISTORY_DAYS", {}, 30, kConfigAnyMin, kConfigAnyMax,
                  ReloadPolicy::kRestartRequired, "retention",
                  "days of saved sessions kept"),

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
    registry::Choice(
        registry::Str("UAGENT_APPROVAL", "approval", "ask",
                      ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                      "behaviour",
                      "ask, auto reviewer, or yolo for ordinary mutations"),
        kApprovalModes),
    registry::Str("UAGENT_PERMISSION_MODEL", "permission_model",
                  "~typesafe/jev-latest", ReloadPolicy::kNextUserTurn,
                  Sensitivity::kPublic, "behaviour",
                  "OpenRouter Decisions model used by auto permissions"),
    registry::Str("UAGENT_PERMISSION_URL", "permission_url",
                  "https://openrouter.ai/api/alpha",
                  ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                  "behaviour", "OpenRouter Decisions API base URL"),
    registry::Str("UAGENT_TITLE_MODEL", "title_model", kDefaultModelRoute,
                  ReloadPolicy::kNextUserTurn, Sensitivity::kPublic,
                  "behaviour", "model route that names new sessions, or off"),
    registry::Str("UAGENT_TOOL_CAPABILITIES", {}, "",
                  ReloadPolicy::kRestartRequired, Sensitivity::kPublic,
                  "behaviour", "restrict the exposed tool capability set"),
    registry::Str(
        "UAGENT_SHELL_ENV_ALLOW", {}, "", ReloadPolicy::kRestartRequired,
        Sensitivity::kPublic, "behaviour",
        "comma-separated sensitive variables approved shells may inherit"),
    registry::Bul("UAGENT_TRUST_PROJECT_CONFIG", {}, false,
                  ReloadPolicy::kRestartRequired, "behaviour",
                  "trust this workspace's .mcp.json and config"),
    registry::Str("UAGENT_CONFIG_FILE", {}, "", ReloadPolicy::kRestartRequired,
                  Sensitivity::kPublic, "behaviour",
                  "replace both config-file locations"),
    registry::Str("UAGENT_DEBUG_LOG", {}, "", ReloadPolicy::kRestartRequired,
                  Sensitivity::kPublic, "behaviour",
                  "write a sensitive reconstructable JSONL trace"),
    registry::Str("UAGENT_USAGE_FILE", {}, "", ReloadPolicy::kRestartRequired,
                  Sensitivity::kPublic, "behaviour",
                  "append per-turn usage records to this path"),
    registry::Bul("UAGENT_MARKDOWN", {}, true, ReloadPolicy::kRestartRequired,
                  "behaviour", "render Markdown on a TTY"),
    registry::Bul("UAGENT_HEADLESS_PROGRESS", {}, false,
                  ReloadPolicy::kRestartRequired, "behaviour",
                  "echo progress lines in headless mode"),
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
bool BoolSetting(const ConfigDescriptor& descriptor);
std::string StringSetting(const ConfigDescriptor& descriptor);
double DoubleSetting(const ConfigDescriptor& descriptor);

const char* ConfigTypeName(ConfigType type);
const char* ReloadPolicyName(ReloadPolicy policy);
const char* SensitivityName(Sensitivity sensitivity);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_CONFIG_REGISTRY_H_
