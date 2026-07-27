// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_ENV_H_
#define UAGENT_INCLUDE_CORE_ENV_H_
// Environment lookups and the parsed runtime configuration. Every tunable
// bound the agent honours is declared here with its default.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "include/core/json.h"
#include "include/core/signals.h"

namespace uagent {

inline std::string EnvStr(const char* name, const std::string& dflt = "") {
  const char* v = getenv(name);
  return (v && *v) ? std::string(v) : dflt;
}

inline int64_t EnvLong(const char* name, int64_t dflt) {
  const char* v = getenv(name);
  if (!v || !*v) return dflt;
  char* end = nullptr;
  int64_t n = strtol(v, &end, 10);
  return (end && *end == '\0') ? n : dflt;
}

inline double EnvDouble(const char* name, double dflt) {
  const char* v = getenv(name);
  if (!v || !*v) return dflt;
  char* end = nullptr;
  errno = 0;
  double n = strtod(v, &end);
  return !errno && end && *end == '\0' ? n : dflt;
}

// Accessors shared by their consumers and the session_ready diagnostic.
inline int64_t ToolResultCap() {
  return EnvLong("UAGENT_TOOL_RESULT_CHARS", 8000);
}
inline int64_t AutoCompactPct() {
  return EnvLong("UAGENT_AUTO_COMPACT_PCT", 95);
}
inline int64_t CheckpointPct() { return EnvLong("UAGENT_CHECKPOINT_PCT", 65); }
inline int64_t CheckpointUrgentPct() {
  return EnvLong("UAGENT_CHECKPOINT_URGENT_PCT", 85);
}
inline int64_t ToolConcurrency() {
  return std::clamp(EnvLong("UAGENT_TOOL_CONCURRENCY", 4), int64_t{1},
                    static_cast<int64_t>(kFgMax));
}
// Delegation depth: 0 is the interactive coordinator. A subagent may delegate
// again while it stays under the cap, so nesting is bounded, not banned.
inline int64_t AgentDepth() {
  return std::max(int64_t{0}, EnvLong("UAGENT_DEPTH", 0));
}
inline bool CanDelegate() {
  return AgentDepth() <
         std::max(int64_t{0}, EnvLong("UAGENT_SUBAGENT_DEPTH", 2));
}
// Budgets handed to a delegated child. Lower than the coordinator's own, so one
// flailing subagent cannot spend the whole turn.
inline int64_t SubagentMaxSteps() {
  return EnvLong("UAGENT_SUBAGENT_MAX_STEPS", 25);
}
inline int64_t SubagentMaxToolCalls() {
  return EnvLong("UAGENT_SUBAGENT_MAX_TOOL_CALLS", 60);
}
inline int64_t MaxOutputTokens() { return EnvLong("UAGENT_MAX_TOKENS", 16000); }
inline bool SteeringEnabled() { return EnvStr("UAGENT_STEERING", "1") != "0"; }

// Core request, MCP, and persistence settings, parsed once after
// ~/.uagent/.config is loaded.
struct RuntimeConfig {
  int64_t first_event_timeout_s = 120;
  int64_t stream_idle_timeout_s = 90;
  int64_t request_timeout_s = 300;
  int64_t request_bytes = 64 * 1024 * 1024;
  int64_t response_bytes = 32 * 1024 * 1024;
  int64_t max_steps = 40;
  int64_t max_tool_calls = 100;
  int64_t max_turn_seconds = 900;
  double max_turn_cost = 1.0;
  int64_t tool_timeout_s = 30;
  int64_t web_search_timeout_s = 25;
  int64_t web_search_max_tokens = 1200;
  int64_t web_search_calls = 4;
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
  std::string checkpoint_mode = "apply";
  std::string openrouter_provider;
  std::string web_search_effort;
  std::string web_search_model;
  bool openrouter_fallbacks = true;

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
      {"UAGENT_CHECKPOINT_MODE", "checkpoint_mode",
       &RuntimeConfig::checkpoint_mode, "apply"},
      {"UAGENT_OPENROUTER_PROVIDER", "openrouter_provider",
       &RuntimeConfig::openrouter_provider, ""},
      {"UAGENT_WEB_SEARCH_EFFORT", "web_search_effort",
       &RuntimeConfig::web_search_effort, ""},
      {"UAGENT_WEB_SEARCH_MODEL", "web_search_model",
       &RuntimeConfig::web_search_model, ""},
  };
  inline static constexpr BoolOption kBoolOptions[] = {
      {"UAGENT_OPENROUTER_FALLBACKS", "openrouter_fallbacks",
       &RuntimeConfig::openrouter_fallbacks, true},
  };

  static RuntimeConfig FromEnvironment() {
    RuntimeConfig c;
    for (const LongOption& option : kLongOptions) {
      c.*option.field = std::clamp(EnvLong(option.env, c.*option.field),
                                   option.minimum, option.maximum);
    }
    for (const StringOption& option : kStringOptions) {
      c.*option.field = EnvStr(option.env, option.default_value);
    }
    for (const BoolOption& option : kBoolOptions) {
      c.*option.field =
          EnvStr(option.env, option.default_value ? "1" : "0") != "0";
    }
    c.max_turn_cost = EnvDouble("UAGENT_MAX_TURN_COST", 1.0);
    if (c.checkpoint_mode != "off" && c.checkpoint_mode != "shadow" &&
        c.checkpoint_mode != "apply") {
      c.checkpoint_mode = "apply";
    }
    return c;
  }

  json DiagnosticJson() const {
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
        {"checkpoint_pct", CheckpointPct()},
        {"checkpoint_urgent_pct", CheckpointUrgentPct()},
        {"auto_compact_pct", AutoCompactPct()},
    });
    return out;
  }
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_ENV_H_
