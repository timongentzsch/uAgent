// Copyright 2026 Timon Gentzsch

#include "include/core/env.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "include/core/config_registry.h"
#include "include/core/limits.h"
#include "include/core/runtime_config.h"
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

int64_t ToolResultCap() { return LongSetting(Cfg("UAGENT_TOOL_RESULT_CHARS")); }

int64_t ToolBatchResultCap() {
  int64_t per_result = ToolResultCap();
  if (per_result <= 0) return per_result;
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  return per_result > kMax / 2 ? kMax : per_result * 2;
}

int64_t AutoCompactPct() { return LongSetting(Cfg("UAGENT_AUTO_COMPACT_PCT")); }

int64_t AutoCompactTokens() {
  return EnvLong("UAGENT_INTERNAL_AUTO_COMPACT_TOKENS", 0);
}

int64_t AgentDepth() { return EnvLong("UAGENT_INTERNAL_DEPTH", 0); }

bool CanDelegate() {
  return AgentDepth() < LongSetting(Cfg("UAGENT_SUBAGENT_DEPTH"));
}

bool LeanToolset() { return EnvStr("UAGENT_INTERNAL_TOOLSET") == "lean"; }

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

std::string CoordinatorModel() {
  return StringSetting(Cfg("UAGENT_COORDINATOR_MODEL"));
}

int64_t SubagentTimeoutSeconds() {
  return LongSetting(Cfg("UAGENT_SUBAGENT_TIMEOUT"));
}

// -1 omits the cap so the provider applies its own maximum; a fixed cap would
// also clamp any thinking budget derived from it.
int64_t MaxOutputTokens() { return LongSetting(Cfg("UAGENT_MAX_TOKENS")); }

bool SandboxEnabled() { return BoolSetting(Cfg("UAGENT_SANDBOX")); }

bool SandboxNetworkAllowed() { return BoolSetting(Cfg("UAGENT_SANDBOX_NET")); }

std::string SandboxWriteRoots() {
  return StringSetting(Cfg("UAGENT_SANDBOX_WRITE"));
}

bool AdaptiveSystemEnabled() { return BoolSetting(Cfg("UAGENT_ADAPT_SYSTEM")); }

bool MarkdownEnabled() { return BoolSetting(Cfg("UAGENT_MARKDOWN")); }

bool TrustProjectConfig() {
  return BoolSetting(Cfg("UAGENT_TRUST_PROJECT_CONFIG"));
}

bool HeadlessProgressEnabled() {
  return EnvStr("UAGENT_INTERNAL_HEADLESS_PROGRESS") == "1";
}

std::string PromptOverlayPath() {
  return EnvStr("UAGENT_INTERNAL_PROMPT_OVERLAY");
}

int64_t ReadFileLines() { return LongSetting(Cfg("UAGENT_READ_FILE_LINES")); }

int64_t AttachmentLimitMb() { return LongSetting(Cfg("UAGENT_ATTACHMENT_MB")); }

int64_t ContextWindow() { return LongSetting(Cfg("UAGENT_CONTEXT")); }

int64_t HistoryDays() { return LongSetting(Cfg("UAGENT_HISTORY_DAYS")); }

std::string ShellEnvironmentAllowlist() {
  return StringSetting(Cfg("UAGENT_SHELL_ENV_ALLOW"));
}

std::atomic<ApprovalMode>& ApprovalModeState() {
  static std::atomic<ApprovalMode> mode{ApprovalMode::kAsk};
  return mode;
}

const char* ApprovalModeName(ApprovalMode mode) {
  switch (mode) {
    case ApprovalMode::kAsk:
      return "ask";
    case ApprovalMode::kAuto:
      return "auto";
    case ApprovalMode::kYolo:
      return "yolo";
  }
  return "ask";
}

bool ParseApprovalMode(std::string_view value, ApprovalMode& mode) {
  if (value.empty() || value == "ask") {
    mode = ApprovalMode::kAsk;
    return true;
  }
  if (value == "auto") {
    mode = ApprovalMode::kAuto;
    return true;
  }
  if (value == "yolo") {
    mode = ApprovalMode::kYolo;
    return true;
  }
  return false;
}

ApprovalMode CurrentApprovalMode() {
  return ApprovalModeState().load(std::memory_order_relaxed);
}

void SetApprovalMode(ApprovalMode mode) {
  ApprovalModeState().store(mode, std::memory_order_relaxed);
}

bool ApprovalIsYolo() { return CurrentApprovalMode() == ApprovalMode::kYolo; }

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
    {&Cfg("UAGENT_STREAM_TIMEOUT"), &RuntimeConfig::stream_timeout_s},
    {&Cfg("UAGENT_REQUEST_TIMEOUT"), &RuntimeConfig::request_timeout_s},
    {&Cfg("UAGENT_MAX_STEPS"), &RuntimeConfig::max_steps},
    {&Cfg("UAGENT_MAX_TOOL_CALLS"), &RuntimeConfig::max_tool_calls},
    {&Cfg("UAGENT_MAX_TURN_SECONDS"), &RuntimeConfig::max_turn_seconds},
    {&Cfg("UAGENT_MAX_TURN_TOKENS"), &RuntimeConfig::max_turn_tokens},
    {&Cfg("UAGENT_SESSION_TOKEN_BUDGET"), &RuntimeConfig::session_token_budget},
    {&Cfg("UAGENT_TOOL_TIMEOUT"), &RuntimeConfig::tool_timeout_s},
    {&Cfg("UAGENT_MCP_TIMEOUT"), &RuntimeConfig::mcp_timeout_s},
};
constexpr FieldBinding<double> kDoubleOptions[] = {
    {&Cfg("UAGENT_MAX_TURN_COST"), &RuntimeConfig::max_turn_cost},
    {&Cfg("UAGENT_SESSION_BUDGET"), &RuntimeConfig::session_budget},
};
constexpr FieldBinding<std::string> kStringOptions[] = {
    {&Cfg("UAGENT_APPROVAL"), &RuntimeConfig::approval},
    {&Cfg("UAGENT_PERMISSION_MODEL"), &RuntimeConfig::permission_model},
    {&Cfg("UAGENT_PERMISSION_URL"), &RuntimeConfig::permission_url},
    {&Cfg("UAGENT_OPENROUTER_PROVIDER"), &RuntimeConfig::openrouter_provider},
    {&Cfg("UAGENT_OPENROUTER_VARIANT"), &RuntimeConfig::openrouter_variant},
    {&Cfg("UAGENT_WEB_SEARCH_BACKEND"), &RuntimeConfig::web_search_backend},
    {&Cfg("UAGENT_WEB_SEARCH_MODEL"), &RuntimeConfig::web_search_model},
    {&Cfg("UAGENT_IMAGE_MODEL"), &RuntimeConfig::image_model},
    {&Cfg("UAGENT_TITLE_MODEL"), &RuntimeConfig::title_model},
    {&Cfg("UAGENT_PDF_ENGINE"), &RuntimeConfig::pdf_engine},
    {&Cfg("UAGENT_MCP_ROOTS"), &RuntimeConfig::mcp_roots},
};
constexpr FieldBinding<bool> kBoolOptions[] = {
    {&Cfg("UAGENT_MEMORY"), &RuntimeConfig::memory_enabled},
    {&Cfg("UAGENT_MEMORY_GENERATE"), &RuntimeConfig::memory_generate},
};

// A fixed-choice setting outside its registered spellings keeps the default.
void NormalizeRuntimeConfig(RuntimeConfig& config) {
  for (const auto& option : kStringOptions) {
    if (!option.descriptor->Accepts(config.*option.field)) {
      config.*option.field =
          std::get<std::string_view>(option.descriptor->default_value);
    }
  }
}

}  // namespace

std::string RuntimeConfigField(std::string_view environment) {
  const ConfigDescriptor* descriptor = FindConfigDescriptor(environment);
  return descriptor ? std::string(descriptor->field) : std::string();
}

namespace {

// One visitor over every RuntimeConfig binding, whatever its type.
template <typename Visit>
void ForEachBinding(Visit&& visit) {
  for (const auto& option : kLongOptions) visit(option);
  for (const auto& option : kDoubleOptions) visit(option);
  for (const auto& option : kStringOptions) visit(option);
  for (const auto& option : kBoolOptions) visit(option);
}

}  // namespace

RuntimeConfig::RuntimeConfig() {
  ForEachBinding([this](const auto& option) {
    using Field = std::remove_reference_t<decltype(this->*option.field)>;
    if constexpr (std::is_same_v<Field, std::string>) {
      this->*option.field =
          std::get<std::string_view>(option.descriptor->default_value);
    } else {
      this->*option.field = std::get<Field>(option.descriptor->default_value);
    }
  });
}

bool ValidOpenRouterVariant(std::string_view variant) {
  return Cfg("UAGENT_OPENROUTER_VARIANT").Accepts(variant);
}

RuntimeConfig RuntimeConfig::FromEnvironment() {
  Values values;
  ForEachBinding([&](const auto& option) {
    if (const char* value = getenv(option.Env())) values[option.Env()] = value;
  });
  return FromValues(values);
}

// An absent, empty or unparsable value keeps the registry default; numbers are
// clamped to the registered bounds.
RuntimeConfig RuntimeConfig::FromValues(const Values& values) {
  RuntimeConfig config;
  ForEachBinding([&](const auto& option) {
    auto found = values.find(option.Env());
    if (found == values.end() || found->second.empty()) return;
    const std::string& text = found->second;
    auto& field = config.*option.field;
    using Field = std::remove_reference_t<decltype(field)>;
    if constexpr (std::is_same_v<Field, int64_t>) {
      int64_t parsed = 0;
      if (ParseInt64(text.c_str(), parsed)) {
        field = std::clamp(parsed, option.descriptor->minimum,
                           option.descriptor->maximum);
      }
    } else if constexpr (std::is_same_v<Field, double>) {
      double parsed = 0;
      if (ParseFiniteDouble(text.c_str(), parsed)) {
        field = std::max(0.0, parsed);
      }
    } else if constexpr (std::is_same_v<Field, bool>) {
      ParseBool(text, field);
    } else {
      field = text;
    }
  });
  NormalizeRuntimeConfig(config);
  return config;
}

std::vector<std::string> RuntimeConfig::ApplyTurnReload(
    const RuntimeConfig& next) {
  std::vector<std::string> changed;
  // Reloadability is a registry property, so the applied set cannot drift from
  // the policy the descriptors publish.
  ForEachBinding([&](const auto& option) {
    if (option.descriptor->reload != ReloadPolicy::kNextUserTurn ||
        this->*option.field == next.*option.field) {
      return;
    }
    this->*option.field = next.*option.field;
    changed.emplace_back(option.descriptor->field);
  });
  return changed;
}

json RuntimeConfig::DiagnosticJson() const {
  json out;
  ForEachBinding([&](const auto& option) {
    const auto& value = this->*option.field;
    if constexpr (std::is_same_v<std::remove_cvref_t<decltype(value)>,
                                 std::string>) {
      const ConfigDescriptor& descriptor = *option.descriptor;
      std::string shown = value;
      if (descriptor.sensitivity != Sensitivity::kPublic) {
        shown = shown.empty() ? "<unset>" : "<set>";
      } else if (descriptor.field.ends_with("_url")) {
        shown = RedactedUrl(std::move(shown));
      }
      out[descriptor.field] = std::move(shown);
    } else {
      out[option.descriptor->field] = value;
    }
  });
  out.update({
      {"auto_compact_pct", AutoCompactPct()},
      {"auto_compact_tokens", AutoCompactTokens()},
  });
  return out;
}

}  // namespace uagent
