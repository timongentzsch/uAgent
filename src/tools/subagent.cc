// Copyright 2026 Timon Gentzsch

#include "include/tools/subagent.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/tools/child_agent.h"
#include "include/tools/jobs.h"
#include "include/tools/shell.h"

namespace uagent {
namespace {

constexpr size_t kAdvertisedRoutes = 16;

// Concurrency is enforced by the spawn path (RunShellCommand reserves an
// activity slot bounded by MaxBackgroundJobs); this is only a runaway ceiling.
int64_t MaxSubagentCallsPerTurn() {
  return std::max<int64_t>(1, EnvLong("UAGENT_SUBAGENT_CALLS_PER_TURN", 32));
}

std::string JoinSelections(std::vector<std::string> selections) {
  std::sort(selections.begin(), selections.end());
  selections.erase(std::unique(selections.begin(), selections.end()),
                   selections.end());
  size_t total = selections.size();
  if (selections.size() > kAdvertisedRoutes) {
    selections.resize(kAdvertisedRoutes);
  }
  std::string result;
  for (const std::string& selection : selections) {
    if (!result.empty()) result += ", ";
    result += selection;
  }
  if (total > selections.size()) {
    result += ", +" + std::to_string(total - selections.size()) + " more";
  }
  return result;
}

std::string ModelPropertyDescription(
    const std::vector<ModelRoute>& routes,
    const std::vector<NamedProvider>& providers) {
  std::vector<std::string> aliases;
  aliases.reserve(routes.size());
  for (const ModelRoute& route : routes) aliases.push_back(route.name);

  std::vector<std::string> prefixes;
  prefixes.reserve(providers.size());
  for (const NamedProvider& provider : providers) {
    prefixes.push_back(provider.name + "/MODEL");
  }

  std::string description =
      "Child model route. Omit for the delegated default shown in runtime "
      "context.";
  std::string configured = JoinSelections(std::move(aliases));
  if (!configured.empty()) description += " Configured: " + configured + ".";
  std::string dynamic = JoinSelections(std::move(prefixes));
  if (!dynamic.empty()) {
    description += " Any model on a named provider: " + dynamic + ".";
  }
  return description;
}

// A delegated child runs on the parent's route unless the request or
// UAGENT_SUBAGENT_MODEL names one; an empty selection tells the shared
// resolver to inherit.
SideRoute ResolveSubagentRoute(const Api& api,
                               const std::vector<ModelRoute>& routes,
                               const std::vector<NamedProvider>& providers,
                               const std::string& requested) {
  return ResolveSideRoute(
      api, routes, providers,
      requested.empty() ? NormalizeModelId(SubagentModel()) : requested);
}

std::string SubagentTargetLabel(const Api& api,
                                const std::vector<ModelRoute>& routes,
                                const std::vector<NamedProvider>& providers,
                                const std::string& requested) {
  return RouteSelection(ResolveSubagentRoute(api, routes, providers, requested),
                        providers);
}

}  // namespace

std::string DefaultSubagentModel(const Api& api) {
  std::string selection = NormalizeModelId(SubagentModel());
  if (!selection.empty()) return selection;
  return api.model;
}

std::string DelegationRuntimeContext(const Api& api) {
  // No provider list reaches here; the built-in templates still scope the
  // common routes, and a custom endpoint degrades to a bare model id.
  std::string parent = TerminalSafe(RouteSelection(api, {}));
  std::string child_model = DefaultSubagentModel(api);
  if (child_model == api.model) {
    return "[delegation: parent=" + parent + "; default=parent]";
  }
  return "[delegation: parent=" + parent +
         "; default=" + TerminalSafe(child_model) + "]";
}

Tool SubagentTool(const Api& api, ProcessSupervisor& processes,
                  const std::vector<ModelRoute>& routes,
                  const std::vector<NamedProvider>& providers, bool debug) {
  json properties = {
      {"prompt",
       {{"type", "string"}, {"description", "complete standalone brief"}}},
      {"background",
       {{"type", "boolean"},
        {"description",
         "default true; false blocks and returns the final result directly"}}},
      {"mode",
       {{"type", "string"},
        {"enum", json::array({"lean", "full"})},
        {"description", "lean default; full includes implementation tools"}}},
      {"model",
       {{"type", "string"},
        {"description", ModelPropertyDescription(routes, providers)}}}};
  Tool tool = MakeTool(
      "subagent",
      "Delegate an isolated subtask whose compact result avoids multiple "
      "parent rounds; for a broad request with orthogonal parts, issue one "
      "task per part in a single batch. The child has no conversation; "
      "include every required path, constraint, and success condition. "
      "Independent tasks may select different model routes. Keep "
      "background=true when useful parent work can continue; set it false "
      "when the next step requires the result immediately. Web-research "
      "briefs must state focused questions and require source URLs in the "
      "final answer.",
      {{"type", "object"},
       {"properties", std::move(properties)},
       {"required", json::array({"prompt"})}},
      [&api, &routes, &providers, debug, &processes](
          const json& arguments, const ToolContext& context) {
        std::string mode = JsonValue(arguments, "mode", "lean");
        if (mode != "lean" && mode != "full") {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "error: mode must be lean or full");
        }
        const std::string requested =
            NormalizeModelId(JsonValue(arguments, "model", ""));
        SideRoute route =
            ResolveSubagentRoute(api, routes, providers, requested);
        if (route.unresolved &&
            route.selection.find('/') != std::string::npos &&
            !CanUseRawModel(api, route.selection)) {
          return ToolFailure(
              ToolErrorCode::kInvalidArguments,
              "error: unknown model route: " + TerminalSafe(route.selection));
        }
        double remaining_budget = 0;
        if (std::optional<ToolResult> blocked =
                ChildAgentBudgetBlock(api, processes, remaining_budget)) {
          return *blocked;
        }
        EnvironmentOverrides environment =
            ChildAgentEnvironment(std::move(route));
        environment.insert(
            environment.end(),
            {{"UAGENT_MAX_STEPS", std::to_string(SubagentMaxSteps())},
             {"UAGENT_MAX_TOOL_CALLS", std::to_string(SubagentMaxToolCalls())},
             {"UAGENT_TOOLSET", std::move(mode)},
             {"UAGENT_MEMORY", api.config.memory_enabled ? "1" : "0"}});
        if (api.config.session_budget > 0) {
          environment.emplace_back("UAGENT_SESSION_BUDGET",
                                   std::to_string(remaining_budget));
        }
        bool background = JsonValue(arguments, "background", true);
        std::string command =
            ChildAgentCommand(debug, JsonValue(arguments, "prompt", ""));
        return RunShellCommand(processes, context,
                               {.command = std::move(command),
                                .background = background,
                                .immediate = background,
                                .job_kind = "subagent",
                                .environment = std::move(environment)})
            .result;
      });
  tool.mutating = true;
  tool.capabilities = Capability(ToolCapability::kDelegate);
  tool.delegates = true;
  tool.retain_output = true;
  tool.available_in_lean = false;
  tool.max_calls_per_turn = MaxSubagentCallsPerTurn();
  tool.summary = [&api, &routes, &providers](const json& arguments) {
    std::string mode = JsonValue(arguments, "mode", "lean");
    std::string prompt = JsonValue(arguments, "prompt", "");
    std::string label = SubagentTargetLabel(
        api, routes, providers,
        NormalizeModelId(JsonValue(arguments, "model", "")));
    if (mode == "full") label += " · full";
    if (!JsonValue(arguments, "background", true)) label += " · foreground";
    return "[" + label + "] " + prompt;
  };
  return tool;  // Spawns serialize; immediate-background children overlap.
}

}  // namespace uagent
