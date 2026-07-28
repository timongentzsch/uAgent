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
  std::string result;
  std::string status;
  std::string label, ordinal;
  double duration_ms = 0;
  bool execute = false;
};

inline void LogToolResult(const CallTask& task, const ToolCall& call,
                          int64_t turn, int64_t step) {
  if (!g_debug.Enabled()) return;
  g_debug.Write("tool_result", {{"turn", turn},
                                {"step", step},
                                {"id", call.id},
                                {"name", call.name},
                                {"status", task.status},
                                {"duration_ms", task.duration_ms},
                                {"result", task.result},
                                {"result_chars", task.result.size()}});
}

inline void ExecuteCall(CallTask& task, const ToolCall& call, int64_t turn,
                        int64_t step, const ToolContext& context,
                        int64_t global_timeout_s) {
  auto started = std::chrono::steady_clock::now();
  if (g_steering.Requested() || AbortRequested()) {
    task.result =
        g_steering.Requested() ? "cancelled by steering" : "cancelled by user";
    task.status = g_steering.Requested() ? "steered" : "cancelled";
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
  task.result =
      CapResult(EscapeToolTags(task.tool->run(arguments, call_context)),
                task.tool->result_chars);
  task.status = g_steering.Requested()
                    ? "steered"
                    : (task.result.starts_with("error:") ? "error" : "ok");
  task.duration_ms = ElapsedMs(started);
  LogToolResult(task, call, turn, step);
}

inline void PrintCallResult(const CallTask& task, const ToolCall& call) {
  std::string safe_result = TerminalSafe(task.result);
  const char* style = task.status == "error" ? RED() : DIM();
  std::string prefix = "  ← " + task.ordinal + TerminalSafe(call.name);
  if (task.tool->full_terminal_output) {
    printf("%s%s%s\n%s\n", style, prefix.c_str(), RST(), safe_result.c_str());
  } else {
    printf("%s%s: %s%s\n", style, prefix.c_str(), safe_result.c_str(), RST());
  }
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_DISPATCH_H_
