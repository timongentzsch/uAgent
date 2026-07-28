// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_SUBAGENT_H_
#define UAGENT_INCLUDE_TOOLS_SUBAGENT_H_
// Delegation as a tool: re-invoke this binary on a scoped sub-task so the
// child's reasoning and tool trace stay in its own context.

#include <string>
#include <vector>

#include "include/api.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/tools/process.h"
#include "include/tools/shell.h"
#include "include/tools/tool.h"

namespace uagent {

// Delegation: re-invoke this same binary on a scoped sub-task. The child's
// reasoning and tool trace stay in its own context and its own log; only the
// final answer comes back, so a wide search costs the coordinator a paragraph
// instead of fifty tool results. The shell runner does the rest — a quick
// sub-task answers inline, a slow one backgrounds itself and is collected by
// pid.
inline Tool SubagentTool(const Api& api, ProcessSupervisor& processes,
                         bool yolo, bool debug) {
  std::string self = g_argv0;
  std::string child_depth = std::to_string(AgentDepth() + 1);
  Tool t = MakeTool(
      "task",
      "Delegate substantial independent research or analysis. Issue one call "
      "per independent subtask in the same response: the spawns serialise but "
      "the children then run concurrently. The child sees no conversation, so "
      "include every path and constraint. Background tasks do not auto-join; "
      "collect them with get_task_output or wait_tasks before the final "
      "answer.",
      json::parse(R"json({"type":"object","properties":{
          "prompt":{"type":"string","description":"complete standalone brief"},
          "run_in_background":{"type":"boolean",
            "description":"return immediately with a task id (default false)"}},
          "required":["prompt"]})json"),
      [self, child_depth, &api, yolo, debug, &processes](
          const json& a, const ToolContext& context) {
        std::string cmd =
            "UAGENT_DEPTH=" + child_depth +
            " UAGENT_MAX_STEPS=" + std::to_string(SubagentMaxSteps()) +
            " UAGENT_MAX_TOOL_CALLS=" + std::to_string(SubagentMaxToolCalls()) +
            " UAGENT_MODEL=" + ShellQuote(api.model) +
            " UAGENT_CONTEXT=" + std::to_string(api.ctx_window) +
            " UAGENT_REASONING_EFFORT=" + ShellQuote(api.reasoning_effort) +
            " UAGENT_USAGE_FILE=" + ShellQuote(UsageLedger()) + " " +
            ShellQuote(self) + (yolo ? " --yolo" : "") +
            (debug ? " --debug" : "") + " -p " +
            ShellQuote(JsonValue(a, "prompt", ""));
        bool background = JsonValue(a, "run_in_background", false);
        return ToolRunBash(processes, cmd, context.timeout_s,
                           /*join_before_final=*/!background, context,
                           /*allow_background=*/true, /*detach=*/false, "bash",
                           background, "task");
      });
  t.mutating = true;
  t.summary = [](const json& a) { return JsonValue(a, "prompt", ""); };
  t.timeout_s = 3;
  // Delegation only overlaps if each spawn backgrounds fast. A model-supplied
  // `timeout` must not stretch the foreground window and serialise the fleet.
  t.max_timeout_s = t.timeout_s;
  return t;  // not parallel_safe: process spawning and sync cancellation are
}  // single-slot, so spawns serialise — the children still overlap

inline void AddTaskLifecycleTools(std::vector<Tool>& tools,
                                  ProcessSupervisor& processes) {
  Tool& get = AddTool(
      tools,
      MakeTool(
          "get_task_output",
          "Get a background task's current bounded output without waiting.",
          json::parse(R"json({"type":"object","properties":{
            "id":{"type":"integer","minimum":1}},"required":["id"]})json"),
          [&processes](const json& a, const ToolContext&) {
            return ToolGetTaskOutput(processes, JsonValue(a, "id", int64_t{0}));
          }));
  get.accepts_timeout = false;
  get.summary = [](const json& a) {
    return "task " + std::to_string(JsonValue(a, "id", int64_t{0}));
  };

  Tool& wait = AddTool(
      tools, MakeTool("wait_tasks",
                      "Wait until any or all selected background tasks finish.",
                      json::parse(R"json({"type":"object","properties":{
            "ids":{"type":"array","items":{"type":"integer","minimum":1},
              "minItems":1,"maxItems":20},
            "wait_all":{"type":"boolean","description":"wait for all; default false"}},
            "required":["ids"]})json"),
                      [&processes](const json& a, const ToolContext& context) {
                        return ToolWaitTasks(
                            processes, JsonValue(a, "ids", json::array()),
                            JsonValue(a, "wait_all", false), context);
                      }));
  wait.summary = [](const json& a) {
    return JsonDump(JsonValue(a, "ids", json::array()));
  };

  Tool& kill = AddTool(
      tools,
      MakeTool("kill_task",
               "Cancel one tracked background task and its process group.",
               json::parse(R"json({"type":"object","properties":{
            "id":{"type":"integer","minimum":1}},"required":["id"]})json"),
               [&processes](const json& a, const ToolContext&) {
                 return ToolKillTask(processes, JsonValue(a, "id", int64_t{0}));
               }));
  kill.accepts_timeout = false;
  kill.mutating = true;
  kill.summary = get.summary;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_SUBAGENT_H_
