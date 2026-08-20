// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_DISPATCH_H_
#define UAGENT_INCLUDE_AGENT_DISPATCH_H_
// One tool call in flight: the state the loop carries for it, how its
// result is traced, and the guarded execution itself.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/protocol.h"
#include "include/api.h"
#include "include/core/checked.h"
#include "include/core/debug.h"
#include "include/core/events.h"
#include "include/core/json.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/tools/tool.h"

namespace uagent {

struct CallTask {
  const Tool* tool = nullptr;
  json args;
  ToolResult result;
  std::string trace_status;
  std::string label, ordinal;
  double duration_ms = 0;
  int64_t completion_order = -1;
  bool execute = false;
  bool started = false;
};

inline int64_t ResultCharLimit(const CallTask& task) {
  if (task.result.result_chars >= 0) return task.result.result_chars;
  return task.tool ? task.tool->result_chars : -1;
}

// Keep recovery metadata at the tail even when a result must be shortened.
// The artifact itself stays out of context; only this small locator rides with
// the result.
inline std::string ModelResultText(const ToolResult& result, int64_t cap) {
  std::string output = EscapeToolTags(result.output);
  if (!result.artifact) return CapResult(std::move(output), cap);
  if (cap < 0) cap = ToolResultCap();
  std::string hint = ArtifactHint(*result.artifact);
  if (cap <= 0) return output + hint;
  size_t limit = static_cast<size_t>(cap);
  if (hint.size() >= limit) return Utf8Prefix(std::move(hint), limit);
  return CapResult(std::move(output), cap - static_cast<int64_t>(hint.size())) +
         hint;
}

// Preserve small results and divide the remaining model-facing budget evenly
// across larger siblings. The original per-tool-capped strings remain intact
// for diagnostics and terminal output.
inline std::vector<std::string> ModelFacingToolResults(
    const std::vector<CallTask>& tasks, int64_t budget = -1) {
  if (budget < 0) {
    budget = ToolBatchResultCap();
    // A tool or per-call result with a larger window also raises the batch
    // window to that amount. Parallel reads therefore share one read-sized
    // budget instead of each being forced back to the smaller generic cap.
    if (budget > 0) {
      for (const CallTask& task : tasks) {
        budget = std::max(budget, ResultCharLimit(task));
      }
    }
  }
  std::vector<std::string> results;
  results.reserve(tasks.size());
  size_t total = 0;
  for (const CallTask& task : tasks) {
    results.push_back(ModelResultText(task.result, ResultCharLimit(task)));
    total = SaturatingAdd(total, results.back().size());
  }
  if (budget <= 0 || total <= static_cast<size_t>(budget)) return results;

  size_t remaining = static_cast<size_t>(budget);
  std::vector<size_t> order(tasks.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t lhs, size_t rhs) {
    if (results[lhs].size() != results[rhs].size()) {
      return results[lhs].size() < results[rhs].size();
    }
    return lhs < rhs;
  });
  std::vector<size_t> caps(tasks.size());
  for (size_t position = 0; position < order.size();) {
    size_t count = order.size() - position;
    size_t share = remaining / count;
    size_t index = order[position];
    if (results[index].size() <= share) {
      caps[index] = results[index].size();
      remaining -= caps[index];
      ++position;
      continue;
    }
    size_t extra = remaining % count;
    for (; position < order.size(); ++position) {
      caps[order[position]] = share;
      if (extra > 0) {
        ++caps[order[position]];
        --extra;
      }
    }
  }
  for (size_t i = 0; i < results.size(); ++i) {
    results[i] = caps[i] == 0 ? ""
                              : ModelResultText(tasks[i].result,
                                                static_cast<int64_t>(caps[i]));
  }
  return results;
}

inline json ToolResultData(const CallTask& task, const ToolCall& call,
                           int64_t turn, int64_t step) {
  return {{"turn", turn},
          {"step", step},
          {"id", call.id},
          {"name", call.name},
          {"status", task.trace_status},
          {"completion_status", CompletionStatusName(task.result.status)},
          {"error_code", ToolErrorCodeName(task.result.error)},
          {"duration_ms", task.duration_ms},
          {"result", task.result.output},
          {"result_chars", task.result.output.size()},
          {"artifact_path",
           task.result.artifact ? task.result.artifact->path : std::string()},
          {"artifact_bytes",
           task.result.artifact ? task.result.artifact->bytes : uint64_t{0}}};
}

inline PresentationRecord ToolResultObservation(const CallTask& task,
                                                const ToolCall& call) {
  PresentationRecord record;
  record.kind = PresentationKind::kToolResult;
  record.id = call.id;
  record.title = task.ordinal + call.name;
  record.status = task.result.status == CompletionStatus::kCancelled
                      ? PresentationStatus::kCancelled
                  : task.result.Ok() ? PresentationStatus::kSucceeded
                                     : PresentationStatus::kFailed;
  record.summary = task.result.output.empty()
                       ? "(empty)"
                       : Utf8Trunc(FirstLine(task.result.output), size_t{512});
  if (task.result.artifact) {
    record.artifacts.push_back({"tool-output", task.result.artifact->path,
                                task.result.artifact->bytes});
  }
  return record;
}

inline void EmitToolResultObservation(const CallTask& task,
                                      const ToolCall& call, int64_t turn,
                                      int64_t step) {
  Event event{EventId::kToolResult, ToolResultData(task, call, turn, step)};
  event.presentation = ToolResultObservation(task, call);
  Emit(std::move(event));
}

inline json ToolCallData(const ToolCall& call, int64_t turn, int64_t step,
                         bool text_protocol) {
  return {{"turn", turn},           {"step", step},
          {"id", call.id},          {"name", call.name},
          {"arguments", call.args}, {"text_protocol", text_protocol}};
}

inline void CancelCall(CallTask& task) {
  bool steered = SteeringState().Requested();
  task.result =
      ToolCancelled(steered ? "cancelled by steering" : "cancelled by user");
  task.trace_status = steered ? "steered" : "cancelled";
}

inline void ExecuteCall(CallTask& task, const ToolCall& call, int64_t turn,
                        int64_t step, const ToolContext& context,
                        int64_t global_timeout_s,
                        std::atomic<int64_t>& completion_sequence) {
  auto started = std::chrono::steady_clock::now();
  task.started = true;
  if (SteeringState().Requested() || AbortRequested()) {
    CancelCall(task);
    task.completion_order =
        completion_sequence.fetch_add(1, std::memory_order_relaxed);
    EmitToolResultObservation(task, call, turn, step);
    return;
  }
  DebugLog("tool_start", {{"turn", turn},
                          {"step", step},
                          {"id", call.id},
                          {"name", call.name},
                          {"arguments", task.args}});
  int64_t timeout =
      task.tool->timeout_s >= 0 ? task.tool->timeout_s : global_timeout_s;
  ToolContext call_context = context.WithTimeout(timeout);
  task.result = task.tool->run(task.args, call_context);
  task.result.output = CapResult(task.result.output, ResultCharLimit(task));
  if (SteeringState().Requested()) {
    task.result.status = CompletionStatus::kCancelled;
    task.result.error = ToolErrorCode::kNone;
    task.trace_status = "steered";
  } else {
    task.trace_status = task.result.Ok() ? "ok" : "error";
  }
  task.duration_ms = ElapsedMs(started);
  task.completion_order =
      completion_sequence.fetch_add(1, std::memory_order_relaxed);
  EmitToolResultObservation(task, call, turn, step);
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_DISPATCH_H_
