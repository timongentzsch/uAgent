// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_RUNTIME_CONFIG_H_
#define UAGENT_INCLUDE_CORE_RUNTIME_CONFIG_H_
// The parsed runtime configuration. Session-static fields live on
// RuntimeConfig; the env-to-field tables live in env.cc.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "include/core/config_registry.h"
#include "include/core/json.h"

namespace uagent {

inline bool ValidOpenRouterVariant(std::string_view variant) {
  return Cfg("UAGENT_OPENROUTER_VARIANT").Accepts(variant);
}

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
  int64_t stream_timeout_s = RegistryDefault<int64_t>("UAGENT_STREAM_TIMEOUT");
  int64_t request_timeout_s =
      RegistryDefault<int64_t>("UAGENT_REQUEST_TIMEOUT");
  int64_t tool_timeout_s = RegistryDefault<int64_t>("UAGENT_TOOL_TIMEOUT");
  int64_t mcp_timeout_s = RegistryDefault<int64_t>("UAGENT_MCP_TIMEOUT");
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
  std::string web_search_model{
      RegistryDefault<std::string_view>("UAGENT_WEB_SEARCH_MODEL")};
  std::string image_model{
      RegistryDefault<std::string_view>("UAGENT_IMAGE_MODEL")};
  std::string title_model{
      RegistryDefault<std::string_view>("UAGENT_TITLE_MODEL")};
  // OpenRouter file-parser engine for documents a model cannot read natively:
  // cloudflare-ai (free), mistral-ocr (scans, billed per page) or native.
  std::string pdf_engine{
      RegistryDefault<std::string_view>("UAGENT_PDF_ENGINE")};
  std::string mcp_roots{RegistryDefault<std::string_view>("UAGENT_MCP_ROOTS")};
  bool memory_enabled = RegistryDefault<bool>("UAGENT_MEMORY");
  bool memory_generate = RegistryDefault<bool>("UAGENT_MEMORY_GENERATE");

  static RuntimeConfig FromEnvironment();
  static RuntimeConfig FromValues(const Values& values);
  std::vector<std::string> ApplyTurnReload(const RuntimeConfig& next);

  json DiagnosticJson() const;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_RUNTIME_CONFIG_H_
