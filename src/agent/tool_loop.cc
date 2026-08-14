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
#include "include/agent/dispatch.h"
#include "include/core/debug.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/ui/tool_output.h"

namespace uagent {
namespace {

size_t ParallelRunEnd(const std::vector<size_t>& runnable,
                      const std::vector<CallTask>& tasks, size_t begin) {
  size_t end = begin;
  while (end < runnable.size() && tasks[runnable[end]].tool->parallel_safe) {
    ++end;
  }
  return end;
}

}  // namespace

void Agent::AppendToolResult(const ToolCall& call, bool text_mode,
                             const std::string& result) {
  const Tool* tool = FindTool(tools_, call.name);
  if (!text_mode && tool && tool->dedupe_output && result.size() >= 256 &&
      conversation_.HasRecentToolResult(call.name, call.args, result)) {
    constexpr char kDuplicate[] =
        "[unchanged duplicate; prior read result remains in recent context]";
    conversation_.Push(
        {{"role", "tool"}, {"tool_call_id", call.id}, {"content", kDuplicate}},
        MessageKind::kToolResult);
    DebugLog("tool_result_deduplicated",
             {{"turn", turn_id_},
              {"name", call.name},
              {"original_chars", result.size()},
              {"model_chars", sizeof(kDuplicate) - 1}});
    return;
  }
  if (text_mode) {
    conversation_.Push(
        HarnessMessage("[tool_result " + call.name + "]\n" + result),
        MessageKind::kToolResult);
  } else {
    conversation_.Push(
        {{"role", "tool"}, {"tool_call_id", call.id}, {"content", result}},
        MessageKind::kToolResult);
  }
}

bool Agent::RunCalls(
    const std::vector<ToolCall>& calls, bool text_mode, int64_t& tool_count,
    std::unordered_map<std::string, int64_t>& tool_counts,
    std::unordered_map<std::string, std::string>& stable_arguments,
    int64_t step, std::chrono::steady_clock::time_point deadline,
    int64_t& consecutive_failed_tools) {
  std::vector<CallTask> tasks(calls.size());
  auto reject = [](CallTask& task, ToolErrorCode code, std::string message,
                   const char* status) {
    task.result = ToolFailure(code, std::move(message));
    task.trace_status = status;
  };
  for (size_t index = 0; index < calls.size(); ++index) {
    const ToolCall& call = calls[index];
    CallTask& task = tasks[index];
    if (calls.size() > 1) {
      task.ordinal = "[" + std::to_string(index + 1) + "] ";
    }
    LogToolCall(call, turn_id_, step, text_mode);
    task.args = json::parse(call.args, nullptr, false);
    task.tool = FindTool(tools_, call.name);
    const Tool* tool = task.tool;
    const json& arguments = task.args;
    std::string invalid;
    bool valid = false;
    if (arguments.is_discarded() || !arguments.is_object()) {
      reject(task, ToolErrorCode::kInvalidArguments,
             "error: malformed tool arguments (not valid JSON)",
             "malformed_arguments");
    } else if (!tool) {
      reject(task, ToolErrorCode::kNotFound, "error: unknown tool " + call.name,
             "unknown_tool");
    } else if (!(invalid = InvalidToolArgument(*tool, arguments)).empty()) {
      reject(task, ToolErrorCode::kInvalidArguments,
             "error: invalid tool argument: " + invalid, "invalid_argument");
    } else if (tool->validate &&
               !(invalid = tool->validate(arguments)).empty()) {
      reject(task, ToolErrorCode::kInvalidArguments, std::move(invalid),
             "rejected");
    } else if (!(invalid =
                     StableArgumentError(*tool, arguments, stable_arguments))
                    .empty()) {
      reject(task, ToolErrorCode::kInvalidArguments, std::move(invalid),
             "unstable_argument");
    } else if (tool->max_calls_per_turn >= 0 &&
               tool_counts[call.name] >= tool->max_calls_per_turn) {
      reject(task, ToolErrorCode::kLimitExceeded,
             "error: " + call.name + " reached its per-turn call limit (" +
                 std::to_string(tool->max_calls_per_turn) +
                 "); answer from the results you have or delegate the "
                 "rest with task — do not reimplement it with run",
             "call_limit");
    } else {
      task.label = ToolSummary(*tool, arguments);
      valid = true;
    }
    PrintToolCall(task, call, verbose_);
    if (valid) {
      bool approval_required =
          ToolMutates(*tool, arguments) ||
          (tool->needs_approval && tool->needs_approval(arguments));
      if (!approval_required || approve_(*tool, arguments)) {
        task.execute = true;
        ++tool_count;
        ++tool_counts[call.name];
      } else {
        reject(task, ToolErrorCode::kPermissionDenied,
               "user denied this action; ask for guidance or try a different "
               "approach",
               "denied");
      }
    }
    if (!task.execute) LogToolResult(task, call, turn_id_, step);
  }

  std::vector<size_t> runnable;
  for (size_t index = 0; index < tasks.size(); ++index) {
    if (tasks[index].execute) runnable.push_back(index);
  }
  int64_t limit = std::max(int64_t{1}, ToolConcurrency());
  if (Debug().Enabled()) {
    // Any two adjacent runnable calls that are both parallel-safe form a batch.
    bool parallel = false;
    for (size_t i = 1; limit > 1 && !parallel && i < runnable.size(); ++i) {
      parallel = tasks[runnable[i - 1]].tool->parallel_safe &&
                 tasks[runnable[i]].tool->parallel_safe;
    }
    Debug().Write("tool_batch", {{"turn", turn_id_},
                                 {"step", step},
                                 {"calls", calls.size()},
                                 {"runnable", runnable.size()},
                                 {"parallel", parallel},
                                 {"concurrency_limit", limit}});
  }

  bool quiet = std::none_of(
      runnable.begin(), runnable.end(),
      [&](size_t index) { return calls[index].name == "show_image"; });
  std::string activity = runnable.size() == 1
                             ? calls[runnable.front()].name
                             : std::to_string(runnable.size()) + " tools";
  TerminalSpinner spinner(!runnable.empty() && quiet, SpinnerLabel(activity),
                          api_.turn_started);
  ToolContext context{deadline};
  for (size_t begin = 0; begin < runnable.size() && !AbortRequested();) {
    if (context.Expired()) break;
    size_t first = runnable[begin];
    // ParallelRunEnd returns `begin` for a call that is not parallel-safe, so
    // both the serial and the lone-safe-call cases advance by one.
    size_t end = limit <= 1 ? begin : ParallelRunEnd(runnable, tasks, begin);
    if (end <= begin + 1) {
      ExecuteCall(tasks[first], calls[first], turn_id_, step, context,
                  api_.config.tool_timeout_s);
      ++begin;
      continue;
    }
    std::atomic<size_t> next{begin};
    size_t workers_count = std::min(end - begin, static_cast<size_t>(limit));
    std::vector<std::future<void>> workers;
    workers.reserve(workers_count);
    for (size_t index = 0; index < workers_count; ++index) {
      workers.push_back(std::async(std::launch::async, [&] {
        for (size_t work; !AbortRequested() && !context.Expired() &&
                          (work = next.fetch_add(1)) < end;) {
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
    if (context.Expired()) {
      task.result = ToolTimedOut("error: turn deadline reached");
      task.trace_status = "timed_out";
    } else {
      CancelCall(task);
    }
    LogToolResult(task, calls[index], turn_id_, step);
  }
  spinner.Stop();

  bool cancelled = AbortRequested() && !SteeringState().Requested();
  if (!SteeringState().Requested()) ClearAbort();
  std::vector<std::string> model_results = ModelFacingToolResults(tasks);
  size_t original_chars = 0;
  size_t model_chars = 0;
  for (size_t index = 0; index < tasks.size(); ++index) {
    const ToolCall& call = calls[index];
    CallTask& task = tasks[index];
    PrintToolResult(task, call, model_results[index], verbose_);
    original_chars = SaturatingAdd(original_chars, task.result.output.size());
    model_chars = SaturatingAdd(model_chars, model_results[index].size());
    AppendToolResult(call, text_mode, model_results[index]);
  }
  bool any_succeeded =
      std::any_of(tasks.begin(), tasks.end(),
                  [](const CallTask& task) { return task.result.Ok(); });
  int64_t failed =
      std::count_if(tasks.begin(), tasks.end(),
                    [](const CallTask& task) { return !task.result.Ok(); });
  consecutive_failed_tools =
      any_succeeded ? 0 : consecutive_failed_tools + failed;
  if (Debug().Enabled() && model_chars < original_chars) {
    Debug().Write("tool_batch_capped", {{"turn", turn_id_},
                                        {"step", step},
                                        {"results", tasks.size()},
                                        {"original_chars", original_chars},
                                        {"model_chars", model_chars}});
  }
  return cancelled;
}

}  // namespace uagent
