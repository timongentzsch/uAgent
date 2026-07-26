// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_SUBAGENT_H_
#define UAGENT_INCLUDE_TOOLS_SUBAGENT_H_
// Delegation as a tool: re-invoke this binary on a scoped sub-task so the
// child's reasoning and tool trace stay in its own context.

#include <string>

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
      "Delegate substantial independent research or analysis. The child sees "
      "no "
      "conversation, so include every path and constraint.",
      json::parse(R"json({"type":"object","properties":{
          "prompt":{"type":"string","description":"complete standalone brief"}},
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
        return ToolRunBash(processes, cmd, context.timeout_s,
                           /*join_before_final=*/true, context);
      });
  t.mutating = true;
  t.summary = [](const json& a) { return JsonValue(a, "prompt", ""); };
  t.timeout_s = 3;
  // Delegation only overlaps if each spawn backgrounds fast. A model-supplied
  // `timeout` must not stretch the foreground window and serialise the fleet.
  t.max_timeout_s = t.timeout_s;
  return t;  // not parallel_safe: process spawning and sync cancellation are
}  // single-slot, so spawns serialise — the children still overlap

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_SUBAGENT_H_
