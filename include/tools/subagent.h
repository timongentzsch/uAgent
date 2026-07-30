// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_SUBAGENT_H_
#define UAGENT_INCLUDE_TOOLS_SUBAGENT_H_
// Delegation as a tool: re-invoke this binary on a scoped sub-task so the
// child's reasoning and tool trace stay in its own context.

#include <string>
#include <utility>
#include <vector>

#include "include/api.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/providers.h"
#include "include/tools/process.h"
#include "include/tools/shell.h"
#include "include/tools/tool.h"

namespace uagent {

// Delegation: re-invoke this same binary on a scoped sub-task. The child's
// reasoning and tool trace stay in its own context and its own log; only the
// final answer comes back, so a wide search costs the coordinator a paragraph
// instead of fifty tool results. Calls spawn immediately, overlap, then join
// before the coordinator's next model step.
inline Tool SubagentTool(const Api& api, ProcessSupervisor& processes,
                         const std::vector<ModelRoute>& routes,
                         const std::vector<NamedProvider>& providers,
                         bool debug) {
  std::string self = g_argv0;
  std::string child_depth = std::to_string(AgentDepth() + 1);
  json properties = {
      {"prompt",
       {{"type", "string"}, {"description", "complete standalone brief"}}},
      {"mode",
       {{"type", "string"},
        {"enum", json::array({"lean", "full"})},
        {"description", "lean default; full includes implementation tools"}}},
      {"model",
       {{"type", "string"},
        {"description",
         "optional model or provider/model selection; default parent"}}}};
  Tool t = MakeTool(
      "task",
      "Delegate an isolated subtask only when its compact result avoids "
      "multiple parent rounds. The child has no conversation; include every "
      "required path, constraint, and success condition. Issue independent "
      "tasks together.",
      {{"type", "object"},
       {"properties", std::move(properties)},
       {"required", json::array({"prompt"})}},
      [self, child_depth, &api, &routes, &providers, debug, &processes](
          const json& a, const ToolContext& context) {
        std::string mode = JsonValue(a, "mode", "lean");
        if (mode != "lean" && mode != "full") {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "error: mode must be lean or full");
        }
        std::string selection = NormalizeModelId(JsonValue(a, "model", ""));
        std::string base_url = api.base_url;
        std::string api_key = api.api_key;
        std::string model = selection.empty() ? api.model : selection;
        std::string effort = api.reasoning_effort;
        int64_t context_window = api.ctx_window;
        if (!selection.empty()) {
          if (std::optional<ModelRoute> route =
                  ResolveModelRoute(routes, providers, selection)) {
            base_url = route->base_url;
            api_key = route->api_key.empty() ? "sk-noop" : route->api_key;
            model = route->model;
            if (!route->effort.empty()) effort = route->effort;
            context_window = route->context;
          } else if (selection.find('/') != std::string::npos &&
                     !CanUseRawModel(api, selection)) {
            return ToolFailure(
                ToolErrorCode::kInvalidArguments,
                "error: unknown model route: " + TerminalSafe(selection));
          }
        }
        EnvironmentOverrides environment = {
            {"UAGENT_DEPTH", child_depth},
            {"UAGENT_MAX_STEPS", std::to_string(SubagentMaxSteps())},
            {"UAGENT_MAX_TOOL_CALLS", std::to_string(SubagentMaxToolCalls())},
            {"UAGENT_BASE_URL", std::move(base_url)},
            {"UAGENT_API_KEY", std::move(api_key)},
            {"UAGENT_MODEL", std::move(model)},
            {"UAGENT_CONTEXT", std::to_string(context_window)},
            {"UAGENT_REASONING_EFFORT", std::move(effort)},
            {"UAGENT_USAGE_FILE", UsageLedger()},
            {"UAGENT_TOOLSET", std::move(mode)},
        };
        std::string cmd = ShellQuote(self) + " --yolo" +
                          (debug ? " --debug" : "") + " -p " +
                          ShellQuote(JsonValue(a, "prompt", ""));
        return ToolRunBash(processes, cmd, context,
                           /*allow_background=*/true, /*detach=*/false, "bash",
                           /*immediate_background=*/true, "task", environment);
      });
  t.mutating = true;
  t.summary = [](const json& a) {
    std::string mode = JsonValue(a, "mode", "lean");
    std::string model = JsonValue(a, "model", "");
    std::string prompt = JsonValue(a, "prompt", "");
    std::string label = mode == "full" ? "full" : "";
    if (!model.empty()) label += (label.empty() ? "" : " · ") + model;
    return label.empty() ? prompt : "[" + label + "] " + prompt;
  };
  return t;  // not parallel_safe: process spawning and sync cancellation are
}  // single-slot, so spawns serialise — the children still overlap

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_SUBAGENT_H_
