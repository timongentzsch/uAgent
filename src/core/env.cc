// Copyright 2026 Timon Gentzsch

#include "include/core/env.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/config_registry.h"
#include "include/core/limits.h"
#include "include/core/signals.h"
#include "include/core/strings.h"

namespace uagent {

std::string EnvStr(const char* name, const std::string& dflt) {
  const char* v = getenv(name);
  return (v && *v) ? std::string(v) : dflt;
}

int64_t EnvLong(const char* name, int64_t dflt) {
  const char* v = getenv(name);
  int64_t value = 0;
  return ParseInt64(v, value) ? value : dflt;
}

double EnvDouble(const char* name, double dflt) {
  const char* v = getenv(name);
  double value = 0;
  return ParseFiniteDouble(v, value) ? value : dflt;
}

namespace {

bool OneOf(std::string_view value,
           std::initializer_list<std::string_view> allowed) {
  return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
}

}  // namespace

int64_t ToolResultCap() { return LongSetting(Cfg("UAGENT_TOOL_RESULT_CHARS")); }

int64_t ToolBatchResultCap() {
  int64_t per_result = ToolResultCap();
  if (per_result <= 0) return per_result;
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  return per_result > kMax / 2 ? kMax : per_result * 2;
}

int64_t ToolTraceProtectChars() {
  return LongSetting(Cfg("UAGENT_TOOL_TRACE_PROTECT_CHARS"));
}

int64_t ToolTracePruneMinChars() {
  return LongSetting(Cfg("UAGENT_TOOL_TRACE_PRUNE_MIN_CHARS"));
}

int64_t AutoCompactPct() { return LongSetting(Cfg("UAGENT_AUTO_COMPACT_PCT")); }

int64_t AutoCompactTokens() {
  return LongSetting(Cfg("UAGENT_AUTO_COMPACT_TOKENS"));
}

int64_t ToolConcurrency() {
  return LongSetting(Cfg("UAGENT_TOOL_CONCURRENCY"));
}

int64_t AgentDepth() { return LongSetting(Cfg("UAGENT_DEPTH")); }

bool CanDelegate() {
  return AgentDepth() < LongSetting(Cfg("UAGENT_SUBAGENT_DEPTH"));
}

bool LeanToolset() { return StringSetting(Cfg("UAGENT_TOOLSET")) == "lean"; }

// The parent runs with no step ceiling at all (RuntimeConfig::max_steps), so a
// child that reads a handful of files per step used to be cut off mid-review
// while its parent was unbounded. Cost, wall clock and the parent's session
// budget are the limits that actually protect a delegated run; these only stop
// a child that has stopped making progress.
int64_t SubagentMaxSteps() {
  return LongSetting(Cfg("UAGENT_SUBAGENT_MAX_STEPS"));
}

int64_t SubagentMaxToolCalls() {
  return LongSetting(Cfg("UAGENT_SUBAGENT_MAX_TOOL_CALLS"));
}

std::string SubagentModel() {
  return StringSetting(Cfg("UAGENT_SUBAGENT_MODEL"));
}

int64_t SubagentTimeoutSeconds() {
  return LongSetting(Cfg("UAGENT_SUBAGENT_TIMEOUT"));
}

int64_t SubagentCallsPerTurn() {
  return LongSetting(Cfg("UAGENT_SUBAGENT_CALLS_PER_TURN"));
}

// -1 omits the cap so the provider applies its own maximum; a fixed cap would
// also clamp any thinking budget derived from it.
int64_t MaxOutputTokens() { return LongSetting(Cfg("UAGENT_MAX_TOKENS")); }

bool SteeringEnabled() { return BoolSetting(Cfg("UAGENT_STEERING")); }

bool AdaptiveSystemEnabled() { return BoolSetting(Cfg("UAGENT_ADAPT_SYSTEM")); }

std::string PromptOverlayPath() {
  return StringSetting(Cfg("UAGENT_PROMPT_OVERLAY"));
}

int64_t ReadFileLines() { return LongSetting(Cfg("UAGENT_READ_FILE_LINES")); }

int64_t ReadFileMaxLines() {
  return LongSetting(Cfg("UAGENT_READ_FILE_MAX_LINES"));
}

int64_t ReadFileBytes() { return LongSetting(Cfg("UAGENT_READ_FILE_BYTES")); }

int64_t ReadFileResultChars() {
  constexpr int64_t kHeaderAllowance = 2048;
  int64_t bytes = ReadFileBytes();
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  return bytes > kMax - kHeaderAllowance ? kMax : bytes + kHeaderAllowance;
}

int64_t EditFileBytes() { return LongSetting(Cfg("UAGENT_EDIT_FILE_BYTES")); }

int64_t ListDirEntries() { return LongSetting(Cfg("UAGENT_LIST_DIR_ENTRIES")); }

int64_t ListDirScanEntries() {
  return LongSetting(Cfg("UAGENT_LIST_DIR_SCAN_ENTRIES"));
}

int64_t MemoryBytes() { return LongSetting(Cfg("UAGENT_MEMORY_BYTES")); }

int64_t MaxMemories() { return LongSetting(Cfg("UAGENT_MEMORY_FILES")); }

int64_t MemoryIdleSeconds() {
  return LongSetting(Cfg("UAGENT_MEMORY_IDLE_SECONDS"));
}

int64_t MemoryExtractBytes() {
  return LongSetting(Cfg("UAGENT_MEMORY_EXTRACT_BYTES"));
}

int64_t SkillBodyBytes() { return LongSetting(Cfg("UAGENT_SKILL_BYTES")); }

int64_t SkillDescriptionBytes() {
  return LongSetting(Cfg("UAGENT_SKILL_DESC_BYTES"));
}

int64_t MaxSkills() { return LongSetting(Cfg("UAGENT_SKILLS")); }

// Defaults to the attachment budget: a document worth fetching is usually one
// worth handing to the model, and a smaller cap here would refuse pages this
// process is already willing to carry. A 2 MiB cap truncated an ordinary
// arXiv paper.
int64_t WebFetchBytes() {
  return LongSetting(Cfg("UAGENT_WEB_FETCH_BYTES"),
                     AttachmentLimitMb() * 1024 * 1024);
}

int64_t GrepResults() { return LongSetting(Cfg("UAGENT_GREP_RESULTS")); }

int64_t GrepBytes() {
  return LongSetting(Cfg("UAGENT_GREP_BYTES"), ToolResultCap());
}

int64_t BashLogBytes() { return LongSetting(Cfg("UAGENT_BASH_LOG_BYTES")); }

int64_t RunDefaultYieldMs() { return LongSetting(Cfg("UAGENT_RUN_YIELD_MS")); }

int64_t MaxBackgroundJobs() {
  return LongSetting(Cfg("UAGENT_MAX_BACKGROUND_JOBS"));
}

int64_t McpConfigBytes() { return LongSetting(Cfg("UAGENT_MCP_CONFIG_BYTES")); }

int64_t McpDescriptionChars() {
  return LongSetting(Cfg("UAGENT_MCP_DESC_CHARS"));
}

int64_t MaxPendingAttachments() {
  return LongSetting(Cfg("UAGENT_PENDING_ATTACHMENTS"));
}

int64_t AttachmentLimitMb() { return LongSetting(Cfg("UAGENT_ATTACHMENT_MB")); }

int64_t TerminalImageLimitMb() {
  return LongSetting(Cfg("UAGENT_TERMINAL_IMAGE_MB"));
}

int64_t ImageMaxColumns() {
  return LongSetting(Cfg("UAGENT_IMAGE_MAX_COLUMNS"));
}

std::string ImageProtocol() {
  return StringSetting(Cfg("UAGENT_IMAGE_PROTOCOL"));
}

int64_t ImageColumns(int64_t available) {
  return LongSetting(Cfg("UAGENT_IMAGE_COLUMNS"), available);
}

int64_t ContextWindow() { return LongSetting(Cfg("UAGENT_CONTEXT")); }

int64_t HistoryDays() { return LongSetting(Cfg("UAGENT_HISTORY_DAYS")); }

int64_t HistoryFiles() { return LongSetting(Cfg("UAGENT_HISTORY_FILES")); }

int64_t DebugDays() { return LongSetting(Cfg("UAGENT_DEBUG_DAYS")); }

int64_t DebugFiles() { return LongSetting(Cfg("UAGENT_DEBUG_FILES")); }

int64_t BgDays() { return LongSetting(Cfg("UAGENT_BG_DAYS")); }

int64_t BgFiles() { return LongSetting(Cfg("UAGENT_BG_FILES")); }

int64_t McpLogDays() { return LongSetting(Cfg("UAGENT_MCP_LOG_DAYS")); }

int64_t McpLogFiles() { return LongSetting(Cfg("UAGENT_MCP_LOG_FILES")); }

int64_t TerminalRecordDays() {
  return LongSetting(Cfg("UAGENT_TERMINAL_DAYS"));
}

std::string ShellEnvironmentAllowlist() {
  return StringSetting(Cfg("UAGENT_SHELL_ENV_ALLOW"));
}

namespace {

// The typed tables now carry only what a descriptor cannot: which RuntimeConfig
// member each setting writes. The descriptor pointer is resolved at compile
// time, so config load does no lookup and a binding naming an unregistered
// setting fails to build.
template <typename T>
struct FieldBinding {
  const ConfigDescriptor* descriptor;
  T RuntimeConfig::* field;

  const char* Env() const { return descriptor->EnvName(); }
};

constexpr FieldBinding<int64_t> kLongOptions[] = {
    {&Cfg("UAGENT_FIRST_EVENT_TIMEOUT"), &RuntimeConfig::first_event_timeout_s},
    {&Cfg("UAGENT_STREAM_IDLE_TIMEOUT"), &RuntimeConfig::stream_idle_timeout_s},
    {&Cfg("UAGENT_REQUEST_TIMEOUT"), &RuntimeConfig::request_timeout_s},
    {&Cfg("UAGENT_REQUEST_BYTES"), &RuntimeConfig::request_bytes},
    {&Cfg("UAGENT_RESPONSE_BYTES"), &RuntimeConfig::response_bytes},
    {&Cfg("UAGENT_MAX_STEPS"), &RuntimeConfig::max_steps},
    {&Cfg("UAGENT_MAX_TOOL_CALLS"), &RuntimeConfig::max_tool_calls},
    {&Cfg("UAGENT_MAX_TURN_SECONDS"), &RuntimeConfig::max_turn_seconds},
    {&Cfg("UAGENT_MAX_TURN_TOKENS"), &RuntimeConfig::max_turn_tokens},
    {&Cfg("UAGENT_SESSION_TOKEN_BUDGET"),
     &RuntimeConfig::session_token_budget},
    {&Cfg("UAGENT_TOOL_TIMEOUT"), &RuntimeConfig::tool_timeout_s},
    {&Cfg("UAGENT_WEB_SEARCH_TIMEOUT"), &RuntimeConfig::web_search_timeout_s},
    {&Cfg("UAGENT_WEB_SEARCH_MAX_TOKENS"),
     &RuntimeConfig::web_search_max_tokens},
    {&Cfg("UAGENT_WEB_SEARCH_CALLS"), &RuntimeConfig::web_search_calls},
    {&Cfg("UAGENT_WEB_SEARCH_MAX_RESULTS"),
     &RuntimeConfig::web_search_max_results},
    {&Cfg("UAGENT_WEB_SEARCH_MAX_USES"), &RuntimeConfig::web_search_max_uses},
    {&Cfg("UAGENT_MCP_TIMEOUT"), &RuntimeConfig::mcp_timeout_s},
    {&Cfg("UAGENT_MCP_STARTUP_GRACE"), &RuntimeConfig::mcp_startup_grace_s},
    {&Cfg("UAGENT_MCP_SERVERS"), &RuntimeConfig::mcp_servers},
    {&Cfg("UAGENT_MCP_PAGES"), &RuntimeConfig::mcp_pages},
    {&Cfg("UAGENT_MCP_TOOLS"), &RuntimeConfig::mcp_tools},
    {&Cfg("UAGENT_MCP_CONFIG_BYTES"), &RuntimeConfig::mcp_config_bytes},
    {&Cfg("UAGENT_MCP_RESPONSE_BYTES"), &RuntimeConfig::mcp_response_bytes},
    {&Cfg("UAGENT_MCP_SCHEMA_BYTES"), &RuntimeConfig::mcp_schema_bytes},
    {&Cfg("UAGENT_MCP_LOG_BYTES"), &RuntimeConfig::mcp_log_bytes},
    {&Cfg("UAGENT_MEMORY_ALWAYS_BYTES"), &RuntimeConfig::memory_always_bytes},
    {&Cfg("UAGENT_PROJECT_DOC_BYTES"), &RuntimeConfig::project_doc_bytes},
    {&Cfg("UAGENT_SESSION_ARCHIVE_BYTES"),
     &RuntimeConfig::session_archive_bytes},
};
constexpr FieldBinding<double> kDoubleOptions[] = {
    {&Cfg("UAGENT_MAX_TURN_COST"), &RuntimeConfig::max_turn_cost},
    {&Cfg("UAGENT_SESSION_BUDGET"), &RuntimeConfig::session_budget},
};
constexpr FieldBinding<std::string> kStringOptions[] = {
    {&Cfg("UAGENT_OPENROUTER_PROVIDER"), &RuntimeConfig::openrouter_provider},
    {&Cfg("UAGENT_OPENROUTER_VARIANT"), &RuntimeConfig::openrouter_variant},
    {&Cfg("UAGENT_WEB_SEARCH_BACKEND"), &RuntimeConfig::web_search_backend},
    {&Cfg("UAGENT_WEB_SEARCH_URL"), &RuntimeConfig::web_search_url},
    {&Cfg("UAGENT_WEB_SEARCH_API_KEY"), &RuntimeConfig::web_search_api_key},
    {&Cfg("UAGENT_WEB_SEARCH_EFFORT"), &RuntimeConfig::web_search_effort},
    {&Cfg("UAGENT_WEB_SEARCH_MODEL"), &RuntimeConfig::web_search_model},
    {&Cfg("UAGENT_WEB_SEARCH_ENGINE"), &RuntimeConfig::web_search_engine},
    {&Cfg("UAGENT_WEB_SEARCH_CONTEXT_SIZE"),
     &RuntimeConfig::web_search_context_size},
    {&Cfg("UAGENT_IMAGE_MODEL"), &RuntimeConfig::image_model},
    {&Cfg("UAGENT_PDF_ENGINE"), &RuntimeConfig::pdf_engine},
    {&Cfg("UAGENT_MCP_ROOTS"), &RuntimeConfig::mcp_roots},
};
constexpr FieldBinding<bool> kBoolOptions[] = {
    {&Cfg("UAGENT_OPENROUTER_FALLBACKS"), &RuntimeConfig::openrouter_fallbacks},
    {&Cfg("UAGENT_MEMORY"), &RuntimeConfig::memory_enabled},
    {&Cfg("UAGENT_MEMORY_GENERATE"), &RuntimeConfig::memory_generate},
};

void NormalizeRuntimeConfig(RuntimeConfig& config) {
  if (!ValidOpenRouterVariant(config.openrouter_variant)) {
    config.openrouter_variant.clear();
  }
  if (!OneOf(config.web_search_backend, {"auto", "openrouter", "off"})) {
    config.web_search_backend = "auto";
  }
  if (!OneOf(config.web_search_engine, {"auto", "native", "exa", "firecrawl",
                                        "parallel", "perplexity"})) {
    config.web_search_engine = "auto";
  }
  if (!OneOf(config.web_search_context_size, {"low", "medium", "high"})) {
    config.web_search_context_size.clear();
  }
}

}  // namespace

std::string RuntimeConfigField(std::string_view environment) {
  const ConfigDescriptor* descriptor = FindConfigDescriptor(environment);
  return descriptor ? std::string(descriptor->field) : std::string();
}

RuntimeConfig RuntimeConfig::FromEnvironment() {
  RuntimeConfig c;
  for (const auto& option : kLongOptions) {
    c.*option.field = LongSetting(*option.descriptor, c.*option.field);
  }
  for (const auto& option : kStringOptions) {
    c.*option.field = StringSetting(*option.descriptor);
  }
  for (const auto& option : kBoolOptions) {
    c.*option.field = BoolSetting(*option.descriptor);
  }
  for (const auto& option : kDoubleOptions) {
    c.*option.field = std::max(0.0, EnvDouble(option.Env(), c.*option.field));
  }
  NormalizeRuntimeConfig(c);
  return c;
}

RuntimeConfig RuntimeConfig::FromValues(const Values& values) {
  RuntimeConfig config;
  auto value = [&](const char* name) -> const std::string* {
    auto found = values.find(name);
    return found == values.end() ? nullptr : &found->second;
  };
  for (const auto& option : kLongOptions) {
    int64_t parsed = config.*option.field;
    const std::string* selected = value(option.Env());
    if (selected) ParseInt64(selected->c_str(), parsed);
    config.*option.field = std::clamp(parsed, option.descriptor->minimum,
                                      option.descriptor->maximum);
  }
  for (const auto& option : kStringOptions) {
    const std::string* selected = value(option.Env());
    config.*option.field = selected ? *selected
                                    : std::string(std::get<std::string_view>(
                                          option.descriptor->default_value));
  }
  for (const auto& option : kBoolOptions) {
    const std::string* selected = value(option.Env());
    config.*option.field =
        selected ? *selected != "0"
                 : std::get<bool>(option.descriptor->default_value);
  }
  for (const auto& option : kDoubleOptions) {
    const std::string* selected = value(option.Env());
    double parsed = 0;
    if (selected && ParseFiniteDouble(selected->c_str(), parsed)) {
      config.*option.field = std::max(0.0, parsed);
    }
  }
  NormalizeRuntimeConfig(config);
  return config;
}

std::vector<std::string> RuntimeConfig::ApplyTurnReload(
    const RuntimeConfig& next) {
  std::vector<std::string> changed;
  // Reloadability is a registry property, so the applied set cannot drift from
  // the policy the descriptors publish.
  auto reload = [&](const auto& options) {
    for (const auto& option : options) {
      if (option.descriptor->reload != ReloadPolicy::kNextUserTurn ||
          this->*option.field == next.*option.field) {
        continue;
      }
      this->*option.field = next.*option.field;
      changed.emplace_back(option.descriptor->field);
    }
  };
  reload(kLongOptions);
  reload(kDoubleOptions);
  reload(kStringOptions);
  reload(kBoolOptions);
  return changed;
}

json RuntimeConfig::ProvenanceJson(const json& env_sources) const {
  json out = json::object();
  auto source = [&](const char* env) {
    return env_sources.is_object() ? JsonValue(env_sources, env, "default")
                                   : std::string("default");
  };
  for (const auto& option : kLongOptions) {
    out[option.descriptor->field] = source(option.Env());
  }
  for (const auto& option : kStringOptions) {
    out[option.descriptor->field] = source(option.Env());
  }
  for (const auto& option : kBoolOptions) {
    out[option.descriptor->field] = source(option.Env());
  }
  for (const auto& option : kDoubleOptions) {
    out[option.descriptor->field] = source(option.Env());
  }
  return out;
}

json RuntimeConfig::DiagnosticJson() const {
  json out;
  for (const auto& option : kLongOptions) {
    out[option.descriptor->field] = this->*option.field;
  }
  for (const auto& option : kStringOptions) {
    const ConfigDescriptor& descriptor = *option.descriptor;
    std::string value = this->*option.field;
    if (descriptor.sensitivity != Sensitivity::kPublic) {
      value = value.empty() ? "<unset>" : "<set>";
    } else if (descriptor.field.ends_with("_url")) {
      value = RedactedUrl(std::move(value));
    }
    out[descriptor.field] = std::move(value);
  }
  for (const auto& option : kBoolOptions) {
    out[option.descriptor->field] = this->*option.field;
  }
  for (const auto& option : kDoubleOptions) {
    out[option.descriptor->field] = this->*option.field;
  }
  out.update({
      {"auto_compact_pct", AutoCompactPct()},
      {"auto_compact_tokens", AutoCompactTokens()},
      {"tool_trace_protect_chars", ToolTraceProtectChars()},
      {"tool_trace_prune_min_chars", ToolTracePruneMinChars()},
  });
  return out;
}

}  // namespace uagent
