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

#include "include/core/json.h"

namespace uagent {

bool ValidOpenRouterVariant(std::string_view variant);

// Core request, MCP, and persistence settings. Bootstrap builds one snapshot;
// a validated turn-boundary reload may replace explicitly safe fields.
std::string RuntimeConfigField(std::string_view environment);

// The limits a turn is measured against. A base of RuntimeConfig rather than a
// member of it so that every `config.max_steps` reader keeps working, and so
// that a turn takes its snapshot by slicing -- one assignment that cannot omit
// a budget the way seven hand-written ones could.
// Defaults are the registry's: the constructor in env.cc reads them through
// the same table that binds each setting to its field, so the two cannot
// disagree and this header needs no registry.
struct TurnBudgets {
  // Zero disables the model-round limit; turn time, cost, context, process,
  // and tool-call budgets remain independent safety limits.
  int64_t max_steps{};
  // Zero disables the aggregate per-turn tool-call budget. Individual tools,
  // repeated identical calls, time, and cost remain bounded.
  int64_t max_tool_calls{};
  // Zero disables the aggregate wall-clock turn deadline. Request, stream,
  // tool, repetition, cost, and user-interrupt limits remain independent.
  int64_t max_turn_seconds{};
  // Zero disables generated-token limits. Enforcement happens between model
  // rounds, so one response may cross a positive ceiling before the turn stops.
  int64_t max_turn_tokens{};
  int64_t session_token_budget{};
  // Zero disables the per-turn reported-cost budget. Users may opt into a
  // positive turn limit or set a separate cumulative session budget.
  double max_turn_cost{};
  double session_budget{};
};

struct RuntimeConfig : TurnBudgets {
  using Values = std::map<std::string, std::string>;
  int64_t stream_timeout_s{};
  int64_t request_timeout_s{};
  int64_t tool_timeout_s{};
  int64_t mcp_timeout_s{};
  std::string approval;
  std::string permission_model;
  std::string permission_url;
  std::string openrouter_provider;
  std::string openrouter_variant;
  std::string web_search_backend;
  std::string web_search_model;
  std::string image_model;
  std::string title_model;
  // OpenRouter file-parser engine for documents a model cannot read natively:
  // cloudflare-ai (free), mistral-ocr (scans, billed per page) or native.
  std::string pdf_engine;
  std::string mcp_roots;
  bool memory_enabled{};
  bool memory_generate{};

  RuntimeConfig();
  static RuntimeConfig FromEnvironment();
  static RuntimeConfig FromValues(const Values& values);
  std::vector<std::string> ApplyTurnReload(const RuntimeConfig& next);

  json DiagnosticJson() const;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_RUNTIME_CONFIG_H_
