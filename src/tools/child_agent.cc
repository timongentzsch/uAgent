// Copyright 2026 Timon Gentzsch

#include "include/tools/child_agent.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/tools/output_buffer.h"

namespace uagent {
namespace {

constexpr size_t kChildDiagnosticBytes = 2048;

const char* FailureStageName(ChildAgentFailureStage stage) {
  switch (stage) {
    case ChildAgentFailureStage::kRouteResolution:
      return "route resolution";
    case ChildAgentFailureStage::kSpawn:
      return "process spawn";
    case ChildAgentFailureStage::kExecution:
      return "child execution";
  }
  return "child execution";
}

const char* FailureRemedy(ChildAgentFailureStage stage) {
  switch (stage) {
    case ChildAgentFailureStage::kRouteResolution:
      return "choose a configured model route or correct UAGENT_PROVIDERS";
    case ChildAgentFailureStage::kSpawn:
      return "verify the uagent executable and local process limits, then "
             "retry this route";
    case ChildAgentFailureStage::kExecution:
      return "inspect the partial diagnostics and verify this endpoint and "
             "model before retrying";
  }
  return "inspect the partial diagnostics before retrying";
}

}  // namespace

std::string ChildAgentFailureReport(std::string_view route,
                                    ChildAgentFailureStage stage,
                                    std::string_view diagnostics) {
  HeadTailBuffer bounded(kChildDiagnosticBytes);
  bounded.Push(TerminalSafe(diagnostics));
  std::string partial = bounded.Snapshot();
  if (partial.empty()) partial = "(none captured)";
  std::string configured = route.empty() ? "(unresolved)" : TerminalSafe(route);
  return "error: delegated child failed\nconfigured route: " + configured +
         "\nfailure stage: " + FailureStageName(stage) +
         "\nremedy: " + FailureRemedy(stage) +
         "\nfallback: none; provider, model, pricing, and privacy policy were "
         "not changed\npartial diagnostics:\n" +
         partial;
}

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
      {"UAGENT_PROVIDER_PROTOCOL", ProviderProtocolName(route.protocol)},
      {"UAGENT_WIRE_API", WireApiName(route.wire_api)},
      {"UAGENT_HOSTED_TOOLS", route.hosted_web_search ? "web_search" : ""},
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
