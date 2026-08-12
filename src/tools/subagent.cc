// Copyright 2026 Timon Gentzsch

#include "include/tools/subagent.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/tools/jobs.h"
#include "include/tools/shell.h"

namespace uagent {
namespace {

constexpr size_t kAdvertisedRoutes = 16;

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

bool InheritsParentRoute(const Api& api, const std::string& requested) {
  return requested.empty() && NormalizeModelId(TaskModel()).empty() &&
         !api.openrouter_compatible;
}

std::string TaskProvider(const std::string& selection,
                         const std::string& base_url,
                         const std::vector<NamedProvider>& providers) {
  size_t slash = selection.find('/');
  if (slash != std::string::npos &&
      FindNamedProvider(providers, selection.substr(0, slash))) {
    return selection.substr(0, slash);
  }
  for (const NamedProvider& provider : providers) {
    if (provider.base_url == base_url) return provider.name;
  }
  const ProviderTemplate* provider = FindProviderTemplateForUrl(base_url);
  return provider ? provider->name : "custom";
}

// Where a delegated task would run: the parent's route unless the request (or
// UAGENT_TASK_MODEL) names one that resolves. `unresolved` marks a named route
// that matched nothing, which only the execution path treats as an error.
struct TaskRoute {
  std::string selection, model, base_url, api_key, effort;
  int64_t context = 0;
  bool openrouter_compatible = false;
  bool unresolved = false;
};

TaskRoute ResolveTaskRoute(const Api& api,
                           const std::vector<ModelRoute>& routes,
                           const std::vector<NamedProvider>& providers,
                           const std::string& requested) {
  TaskRoute resolved;
  resolved.selection = requested.empty() ? DefaultTaskModel(api) : requested;
  resolved.model = resolved.selection.empty() ? api.model : resolved.selection;
  resolved.base_url = api.base_url;
  resolved.api_key = api.api_key;
  resolved.effort = api.reasoning_effort;
  resolved.context = api.ctx_window;
  resolved.openrouter_compatible = api.openrouter_compatible;
  if (resolved.selection.empty() || InheritsParentRoute(api, requested)) {
    return resolved;
  }
  std::optional<ModelRoute> route =
      ResolveModelRoute(routes, providers, resolved.selection);
  if (!route) {
    resolved.unresolved = true;
    return resolved;
  }
  resolved.base_url = route->base_url;
  resolved.api_key = route->api_key.empty() ? "sk-noop" : route->api_key;
  resolved.model = route->model;
  if (!route->effort.empty()) resolved.effort = route->effort;
  resolved.context = route->context;
  resolved.openrouter_compatible = route->openrouter_compatible;
  return resolved;
}

std::string TaskTargetLabel(const Api& api,
                            const std::vector<ModelRoute>& routes,
                            const std::vector<NamedProvider>& providers,
                            const std::string& requested) {
  TaskRoute route = ResolveTaskRoute(api, routes, providers, requested);
  return ModelLabel(route.model, route.effort) + " @ " +
         TaskProvider(route.selection, route.base_url, providers);
}

}  // namespace

std::string DefaultTaskModel(const Api& api) {
  std::string selection = NormalizeModelId(TaskModel());
  if (!selection.empty()) return selection;
  if (api.openrouter_compatible) return "deepseek/deepseek-v4-flash";
  return api.model;
}

std::string DelegationRuntimeContext(const Api& api) {
  return "[delegation: parent=" +
         TerminalSafe(ModelLabel(api.model, api.reasoning_effort)) +
         "; default=" +
         TerminalSafe(ModelLabel(DefaultTaskModel(api), api.reasoning_effort)) +
         "]";
}

Tool SubagentTool(const Api& api, ProcessSupervisor& processes,
                  const std::vector<ModelRoute>& routes,
                  const std::vector<NamedProvider>& providers, bool debug) {
  const std::string& self = ExecutablePath();
  std::string child_depth = std::to_string(AgentDepth() + 1);
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
      "task",
      "Delegate an isolated subtask only when its compact result avoids "
      "multiple parent rounds. The child has no conversation; include every "
      "required path, constraint, and success condition. Independent tasks "
      "may select different model routes and should be issued together. Keep "
      "background=true when useful parent work can continue; set it false "
      "when the next step requires the result immediately. Web-research "
      "briefs must state focused questions and require source URLs in the "
      "final answer.",
      {{"type", "object"},
       {"properties", std::move(properties)},
       {"required", json::array({"prompt"})}},
      [self, child_depth, &api, &routes, &providers, debug, &processes](
          const json& arguments, const ToolContext& context) {
        std::string mode = JsonValue(arguments, "mode", "lean");
        if (mode != "lean" && mode != "full") {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "error: mode must be lean or full");
        }
        const std::string requested =
            NormalizeModelId(JsonValue(arguments, "model", ""));
        TaskRoute route = ResolveTaskRoute(api, routes, providers, requested);
        if (route.unresolved &&
            route.selection.find('/') != std::string::npos &&
            !CanUseRawModel(api, route.selection)) {
          return ToolFailure(
              ToolErrorCode::kInvalidArguments,
              "error: unknown model route: " + TerminalSafe(route.selection));
        }
        double remaining_budget = api.config.session_budget - api.session_cost;
        if (api.config.session_budget > 0) {
          if (remaining_budget <= 0) {
            return ToolFailure(ToolErrorCode::kLimitExceeded,
                               "error: session cost limit reached");
          }
          if (processes.JoinableCount() > 0) {
            return ToolFailure(
                ToolErrorCode::kLimitExceeded,
                "error: budgeted task already running; wait for its result");
          }
        }
        EnvironmentOverrides environment = {
            {"UAGENT_DEPTH", child_depth},
            {"UAGENT_MAX_STEPS", std::to_string(SubagentMaxSteps())},
            {"UAGENT_MAX_TOOL_CALLS", std::to_string(SubagentMaxToolCalls())},
            {"UAGENT_BASE_URL", std::move(route.base_url)},
            {"UAGENT_API_KEY", std::move(route.api_key)},
            {"UAGENT_MODEL", std::move(route.model)},
            {"UAGENT_CONTEXT", std::to_string(route.context)},
            {"UAGENT_REASONING_EFFORT", std::move(route.effort)},
            {"UAGENT_OPENROUTER_COMPATIBLE",
             route.openrouter_compatible ? "1" : "0"},
            {"UAGENT_OPENROUTER_VARIANT", api.config.openrouter_variant},
            {"UAGENT_USAGE_FILE", UsageLedger()},
            {"UAGENT_TOOLSET", std::move(mode)},
            {"UAGENT_MEMORY", api.config.memory_enabled ? "1" : "0"},
        };
        if (api.config.session_budget > 0) {
          environment.emplace_back("UAGENT_SESSION_BUDGET",
                                   std::to_string(remaining_budget));
        }
        bool background = JsonValue(arguments, "background", true);
        std::string command = ShellQuote(self) + " --yolo" +
                              (debug ? " --debug" : "") + " -p " +
                              ShellQuote(JsonValue(arguments, "prompt", ""));
        return RunShellCommand(processes, context,
                               {.command = std::move(command),
                                .background = background,
                                .immediate = background,
                                .job_kind = "task",
                                .environment = std::move(environment)})
            .result;
      });
  tool.mutating = true;
  tool.capabilities = Capability(ToolCapability::kDelegate);
  tool.retain_output = true;
  tool.summary = [&api, &routes, &providers](const json& arguments) {
    std::string mode = JsonValue(arguments, "mode", "lean");
    std::string prompt = JsonValue(arguments, "prompt", "");
    std::string label =
        TaskTargetLabel(api, routes, providers,
                        NormalizeModelId(JsonValue(arguments, "model", "")));
    if (mode == "full") label += " · full";
    if (!JsonValue(arguments, "background", true)) label += " · foreground";
    return "[" + label + "] " + prompt;
  };
  return tool;  // Spawns serialize; immediate-background children overlap.
}

}  // namespace uagent
