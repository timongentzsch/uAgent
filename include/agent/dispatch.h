// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_DISPATCH_H_
#define UAGENT_INCLUDE_AGENT_DISPATCH_H_
// One tool call in flight: the state the loop carries for it, how its
// result is traced, and the guarded execution itself.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#include "include/agent/protocol.h"
#include "include/api.h"
#include "include/core/debug.h"
#include "include/core/json.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/tools/tool.h"

namespace uagent {

struct CallTask {
  const Tool* tool = nullptr;
  json args;
  ToolResult result;
  std::string trace_status;
  std::string label, ordinal;
  double duration_ms = 0;
  bool execute = false;
};

inline void LogToolResult(const CallTask& task, const ToolCall& call,
                          int64_t turn, int64_t step) {
  if (!g_debug.Enabled()) return;
  g_debug.Write(
      "tool_result",
      {{"turn", turn},
       {"step", step},
       {"id", call.id},
       {"name", call.name},
       {"status", task.trace_status},
       {"completion_status", CompletionStatusName(task.result.status)},
       {"error_code", ToolErrorCodeName(task.result.error)},
       {"detail", task.trace_status},
       {"duration_ms", task.duration_ms},
       {"result", task.result.output},
       {"result_chars", task.result.output.size()}});
}

inline void ExecuteCall(CallTask& task, const ToolCall& call, int64_t turn,
                        int64_t step, const ToolContext& context,
                        int64_t global_timeout_s) {
  auto started = std::chrono::steady_clock::now();
  if (g_steering.Requested() || AbortRequested()) {
    task.result = ToolCancelled(g_steering.Requested() ? "cancelled by steering"
                                                       : "cancelled by user");
    task.trace_status = g_steering.Requested() ? "steered" : "cancelled";
    LogToolResult(task, call, turn, step);
    return;
  }
  if (g_debug.Enabled()) {
    g_debug.Write("tool_start", {{"turn", turn},
                                 {"step", step},
                                 {"id", call.id},
                                 {"name", call.name},
                                 {"arguments", task.args}});
  }
  int64_t timeout = 0;
  if (task.tool->accepts_timeout) {
    timeout =
        task.tool->timeout_s >= 0 ? task.tool->timeout_s : global_timeout_s;
    timeout = JsonInt(task.args, "timeout", timeout);
  }
  // A tool with a deliberate foreground window (delegation, which must
  // background fast to overlap) caps what the model can ask for; 0 means
  // "turn limit", so it is capped too.
  if (task.tool->max_timeout_s >= 0 &&
      (timeout <= 0 || timeout > task.tool->max_timeout_s)) {
    timeout = task.tool->max_timeout_s;
  }
  ToolContext call_context = context.WithTimeout(timeout);
  json arguments = task.args;
  arguments.erase("timeout");  // runtime policy, never a provider argument
  task.result = task.tool->run(arguments, call_context);
  task.result.output =
      CapResult(EscapeToolTags(task.result.output), task.tool->result_chars);
  if (g_steering.Requested()) {
    task.result.status = CompletionStatus::kCancelled;
    task.result.error = ToolErrorCode::kNone;
    task.trace_status = "steered";
  } else {
    task.trace_status = task.result.Ok() ? "ok" : "error";
  }
  task.duration_ms = ElapsedMs(started);
  LogToolResult(task, call, turn, step);
}

inline void PrintCallResult(const CallTask& task, const ToolCall& call) {
  std::string safe_result = TerminalSafe(task.result.output);
  const char* style = task.result.status == CompletionStatus::kFailed ||
                              task.result.status == CompletionStatus::kTimedOut
                          ? RED()
                          : DIM();
  std::string prefix = "  ← " + task.ordinal + TerminalSafe(call.name);
  if (task.tool->full_terminal_output) {
    printf("%s%s%s\n%s\n", style, prefix.c_str(), RST(), safe_result.c_str());
  } else {
    printf("%s%s: %s%s\n", style, prefix.c_str(), safe_result.c_str(), RST());
  }
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_DISPATCH_H_
