// Copyright 2026 Timon Gentzsch

#include "include/tools/child_agent.h"

#include <string>
#include <utility>

#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/signals.h"
#include "include/core/strings.h"

namespace uagent {

EnvironmentOverrides ChildAgentEnvironment(SideRoute route) {
  return {
      {"UAGENT_DEPTH", std::to_string(AgentDepth() + 1)},
      {"UAGENT_BASE_URL", std::move(route.base_url)},
      {"UAGENT_API_KEY", std::move(route.api_key)},
      {"UAGENT_MODEL", std::move(route.model)},
      {"UAGENT_CONTEXT", std::to_string(route.context)},
      {"UAGENT_REASONING_EFFORT", std::move(route.effort)},
      {"UAGENT_OPENROUTER_COMPATIBLE",
       route.protocol == ProviderProtocol::kOpenRouter ? "1" : "0"},
      {"UAGENT_OPENROUTER_VARIANT", std::move(route.variant)},
      {"UAGENT_USAGE_FILE", UsageLedger()},
  };
}

std::string ChildAgentCommand(bool debug, const std::string& prompt) {
  return ShellQuote(ExecutablePath()) + " --yolo" + (debug ? " --debug" : "") +
         " -p " + ShellQuote(prompt);
}

std::optional<ToolResult> ChildAgentBudgetBlock(
    const Api& api, const ProcessSupervisor& processes, double& remaining) {
  remaining = api.config.session_budget - api.session_cost;
  if (api.config.session_budget <= 0) return std::nullopt;
  if (remaining <= 0) {
    return ToolFailure(ToolErrorCode::kLimitExceeded,
                       "error: session cost limit reached");
  }
  if (processes.JoinableCount() > 0) {
    return ToolFailure(
        ToolErrorCode::kLimitExceeded,
        "error: budgeted child already running; wait for its result");
  }
  return std::nullopt;
}

}  // namespace uagent
