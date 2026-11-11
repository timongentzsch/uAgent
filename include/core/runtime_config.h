// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_RUNTIME_CONFIG_H_
#define UAGENT_INCLUDE_CORE_RUNTIME_CONFIG_H_
// The parsed runtime configuration; the env-to-field tables live in env.cc.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "include/core/json.h"

namespace uagent {

// The RuntimeConfig field a setting binds to; empty when it binds to none.
std::string RuntimeConfigField(std::string_view environment);

// The limits a turn is measured against; zero disables one. A base of
// RuntimeConfig so a turn takes its snapshot by slicing.
struct TurnBudgets {
  int64_t max_steps{};
  int64_t max_tool_calls{};
  int64_t max_turn_seconds{};
  // Enforced between model rounds, so one response may cross the ceiling.
  int64_t max_turn_tokens{};
  int64_t session_token_budget{};
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
  std::string web_search_backend;
  std::string web_search_model;
  std::string image_model;
  std::string title_model;
  bool memory_enabled{};
  bool memory_generate{};

  RuntimeConfig();
  // From what is in effect now: the published settings, or the environment
  // where nothing is published yet.
  static RuntimeConfig FromEnvironment();
  static RuntimeConfig FromValues(const Values& values);
  std::vector<std::string> ApplyTurnReload(const RuntimeConfig& next);

  json DiagnosticJson() const;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_RUNTIME_CONFIG_H_
