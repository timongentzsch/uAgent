// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "include/agent.h"

namespace uagent {

void Agent::AppendToolResult(const ToolCall& call, bool text_mode,
                             const std::string& result) {
  if (text_mode) {
    conversation_.Push(
        {{"role", "user"},
         {"content", "[tool_result " + call.name + "]\n" + result}},
        MessageKind::kToolResult);
  } else {
    conversation_.Push(
        {{"role", "tool"}, {"tool_call_id", call.id}, {"content", result}},
        MessageKind::kToolResult);
  }
}

std::vector<std::string> Agent::RecentToolResults(int64_t count) const {
  return conversation_.RecentToolResults(count);
}

bool Agent::RunCalls(const std::vector<ToolCall>& calls, bool text_mode,
                     int64_t& tool_count,
                     std::unordered_map<std::string, int64_t>& tool_counts,
                     int64_t step,
                     std::chrono::steady_clock::time_point deadline) {
  if (calls.size() == 1 && calls[0].name == "checkpoint") {
    return RunCheckpointCall(calls[0], text_mode, tool_count, step);
  }
  if (pending_checkpoint_.is_object() &&
      JsonValue(pending_checkpoint_, "turn", int64_t{-1}) == turn_id_) {
    InvalidatePendingCheckpoint("tool call followed checkpoint");
  }

  std::vector<CallTask> tasks(calls.size());
  for (size_t index = 0; index < calls.size(); ++index) {
    const ToolCall& call = calls[index];
    CallTask& task = tasks[index];
    if (calls.size() > 1) {
      task.ordinal = "[" + std::to_string(index + 1) + "] ";
    }
    if (g_debug.Enabled()) {
      g_debug.Write("tool_call", {{"turn", turn_id_},
                                  {"step", step},
                                  {"id", call.id},
                                  {"name", call.name},
                                  {"arguments", call.args},
                                  {"text_protocol", text_mode}});
    }
    task.args = json::parse(call.args, nullptr, false);
    task.tool = FindTool(tools_, call.name);
    const Tool* tool = task.tool;
    const json& arguments = task.args;
    std::string invalid;
    if (arguments.is_discarded() || !arguments.is_object()) {
      task.result =
          ToolFailure(ToolErrorCode::kInvalidArguments,
                      "error: malformed tool arguments (not valid JSON)");
      task.trace_status = "malformed_arguments";
    } else if (!tool) {
      task.result = ToolFailure(ToolErrorCode::kNotFound,
                                "error: unknown tool " + call.name);
      task.trace_status = "unknown_tool";
    } else if (!(invalid = MissingRequired(*tool, arguments)).empty()) {
      task.result =
          ToolFailure(ToolErrorCode::kInvalidArguments,
                      "error: missing required argument `" + invalid + "`");
      task.trace_status = "missing_argument";
    } else if (!(invalid = InvalidArgumentType(*tool, arguments)).empty()) {
      task.result = ToolFailure(ToolErrorCode::kInvalidArguments,
                                "error: invalid tool argument: " + invalid);
      task.trace_status = "invalid_argument";
    } else if (call.name == "checkpoint") {
      task.result = ToolFailure(
          ToolErrorCode::kInvalidArguments,
          "error: checkpoint must be the only call in its tool batch");
      task.trace_status = "invalid_batch";
    } else if (tool->max_calls_per_turn >= 0 &&
               tool_counts[call.name] >= tool->max_calls_per_turn) {
      task.result = ToolFailure(
          ToolErrorCode::kLimitExceeded,
          "error: " + call.name + " reached its per-turn call limit (" +
              std::to_string(tool->max_calls_per_turn) +
              "); answer from the results you have or delegate the "
              "rest with task — do not reimplement it with run");
      task.trace_status = "call_limit";
    } else {
      task.label = ToolSummary(*tool, arguments);
      std::string prefix = "→ " + task.ordinal + TerminalSafe(call.name);
      if (tool->full_terminal_output) {
        printf("%s%s%s\n%s\n", CYAN(), prefix.c_str(), RST(),
               TerminalSafe(task.label).c_str());
      } else {
        printf("%s%s(%s)%s\n", CYAN(), prefix.c_str(),
               TerminalSafe(FirstLine(task.label)).c_str(), RST());
      }
      bool approval_required =
          tool->mutating ||
          (tool->needs_approval && tool->needs_approval(arguments));
      if (!approval_required || approve_(*tool, arguments)) {
        task.execute = true;
        ++tool_count;
        ++tool_counts[call.name];
      } else {
        task.result = ToolFailure(
            ToolErrorCode::kPermissionDenied,
            "user denied this action; ask for guidance or try a different "
            "approach");
        task.trace_status = "denied";
        printf("%s  denied%s\n", RED(), RST());
      }
    }
    if (!task.execute) LogToolResult(task, call, turn_id_, step);
  }

  std::vector<size_t> runnable;
  for (size_t index = 0; index < tasks.size(); ++index) {
    if (tasks[index].execute) runnable.push_back(index);
  }
  int64_t limit = std::max(int64_t{1}, ToolConcurrency());
  bool parallel = false;
  if (limit > 1) {
    for (size_t begin = 0; begin < runnable.size();) {
      if (!tasks[runnable[begin]].tool->parallel_safe) {
        ++begin;
        continue;
      }
      size_t end = begin;
      while (end < runnable.size() &&
             tasks[runnable[end]].tool->parallel_safe) {
        ++end;
      }
      parallel = parallel || end - begin > 1;
      begin = end;
    }
  }
  if (g_debug.Enabled()) {
    g_debug.Write("tool_batch", {{"turn", turn_id_},
                                 {"step", step},
                                 {"calls", calls.size()},
                                 {"runnable", runnable.size()},
                                 {"parallel", parallel},
                                 {"concurrency_limit", limit}});
  }

  SteeringGuard steering(!runnable.empty());
  bool quiet = std::none_of(
      runnable.begin(), runnable.end(),
      [&](size_t index) { return calls[index].name == "show_image"; });
  std::string activity = runnable.size() == 1
                             ? calls[runnable.front()].name
                             : std::to_string(runnable.size()) + " tools";
  TerminalSpinner spinner(!runnable.empty() && quiet,
                          SpinnerLabel(std::move(activity)));
  ToolContext context{deadline};
  for (size_t begin = 0; begin < runnable.size() && !AbortRequested();) {
    size_t first = runnable[begin];
    if (limit <= 1 || !tasks[first].tool->parallel_safe) {
      ExecuteCall(tasks[first], calls[first], turn_id_, step, context,
                  api_.config.tool_timeout_s);
      ++begin;
      continue;
    }
    size_t end = begin;
    while (end < runnable.size() && tasks[runnable[end]].tool->parallel_safe) {
      ++end;
    }
    if (end - begin == 1) {
      ExecuteCall(tasks[first], calls[first], turn_id_, step, context,
                  api_.config.tool_timeout_s);
      begin = end;
      continue;
    }
    std::atomic<size_t> next{begin};
    size_t workers_count = std::min(end - begin, static_cast<size_t>(limit));
    std::vector<std::future<void>> workers;
    for (size_t index = 0; index < workers_count; ++index) {
      workers.push_back(std::async(std::launch::async, [&] {
        for (size_t work;
             !AbortRequested() && (work = next.fetch_add(1)) < end;) {
          size_t call_index = runnable[work];
          ExecuteCall(tasks[call_index], calls[call_index], turn_id_, step,
                      context, api_.config.tool_timeout_s);
        }
      }));
    }
    for (auto& worker : workers) worker.get();
    begin = end;
  }
  for (size_t index : runnable) {
    CallTask& task = tasks[index];
    if (task.started) continue;
    task.result = ToolCancelled(g_steering.Requested() ? "cancelled by steering"
                                                       : "cancelled by user");
    task.trace_status = g_steering.Requested() ? "steered" : "cancelled";
    LogToolResult(task, calls[index], turn_id_, step);
  }
  spinner.Stop();
  steering.Stop();

  bool cancelled = AbortRequested() && !g_steering.Requested();
  if (!g_steering.Requested()) ClearAbort();
  for (size_t index = 0; index < tasks.size(); ++index) {
    const ToolCall& call = calls[index];
    CallTask& task = tasks[index];
    if (task.execute) PrintCallResult(task, call);
    RecordSideEffect(task, call);
    AppendToolResult(call, text_mode, task.result.output);
  }
  return cancelled;
}

}  // namespace uagent
