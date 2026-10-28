// Copyright 2026 Timon Gentzsch

#include "include/agent/dispatch.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/activity.h"
#include "include/agent/protocol.h"
#include "include/api/client.h"
#include "include/core/checked.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/events.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/tools/tool.h"

namespace uagent {

int64_t ResultCharLimit(const CallTask& task) {
  if (task.result.result_chars >= 0) return task.result.result_chars;
  return task.tool ? task.tool->result_chars : -1;
}

// Keep the hints at the tail even when a result must be shortened: the
// artifact itself stays out of context; only its small locator rides with the
// result.
std::string ModelResultText(const ToolResult& result, int64_t cap) {
  std::string output = result.output;
  if (cap < 0) cap = ToolResultCap();
  std::string hint = ModelHint(result);
  if (hint.empty()) return LimitOutput(std::move(output), cap);
  if (cap <= 0) return output + hint;
  size_t limit = static_cast<size_t>(cap);
  if (hint.size() >= limit) return Utf8Prefix(std::move(hint), limit);
  return LimitOutput(std::move(output),
                     cap - static_cast<int64_t>(hint.size())) +
         hint;
}

// Preserve small results and divide the remaining model-facing budget evenly
// across larger siblings. The original per-tool-capped strings remain intact
// for diagnostics and terminal output.
std::vector<std::string> ModelFacingToolResults(
    const std::vector<CallTask>& tasks, int64_t budget) {
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

json ToolResultData(const CallTask& task, const ToolCall& call, int64_t turn,
                    int64_t step) {
  const char* state = ResultState(task.result.status);
  json data = {
      {"turn", turn},
      {"step", step},
      {"call_id", call.id},
      {"response_id", call.response_id},
      {"occurrence_id", call.occurrence_id},
      {"detail_id", call.detail_id},
      {"name", call.name},
      {"activity", task.activity},
      {"headline", task.headline},
      {"status", task.trace_status},
      {"completion_status", CompletionStatusName(task.result.status)},
      {"error_code", ToolErrorCodeName(task.result.error)},
      {"issue_code", task.issue ? task.issue->code : std::string()},
      {"issue_field", task.issue ? task.issue->field : std::string()},
      {"duration_ms", task.duration_ms},
      {"result", task.result.output},
      {"hint", task.result.hint},
      {"result_chars", task.result.output.size()},
      {"state", state},
      {"facts", ResultJson(task.result.facts, state, task.duration_ms)},
      {"body_kind", task.result.body_kind},
      {"no_change", task.result.facts.no_change},
      {"activity_terminal", task.result.activity_terminal},
      {"artifact_path",
       task.result.artifact ? task.result.artifact->path : std::string()},
      {"artifact_bytes",
       task.result.artifact ? task.result.artifact->bytes : uint64_t{0}}};
  if (task.result.parts.is_array()) data["parts"] = task.result.parts;
  // The operation is a schema enum, not an argument value. It is enough to
  // distinguish a quiet poll from a wait without journalling ids or input.
  if (call.name == "activity" && task.args.is_object()) {
    data["activity_operation"] = JsonValue(task.args, "operation", "");
  }
  return data;
}

namespace {
// Why the loop ended a call as it did, beside its state.
std::string Outcome(const CallTask& task) {
  const std::string& status = task.trace_status;
  if (status == "denied" || status == "rejected") return status;
  if (status == "invalid_argument" || status == "malformed_arguments") {
    return "invalid";
  }
  if (status == "call_limit") return "limit";
  if (status == "unknown_tool" || status == "inactive_tool") {
    return "unavailable";
  }
  if (task.result.status == CompletionStatus::kTimedOut) return "timed_out";
  return task.result.facts.outcome;
}
}  // namespace

void EmitToolResultObservation(CallTask& task, const ToolCall& call,
                               int64_t turn, int64_t step) {
  ResultFacts& facts = task.result.facts;
  facts.outcome = Outcome(task);
  if (task.issue &&
      (facts.outcome == "rejected" || facts.outcome == "invalid")) {
    facts.reason = Utf8Trunc(FirstLine(task.issue->message), 80);
  }
  Emit(Event{EventId::kToolResult, ToolResultData(task, call, turn, step)});
}

json ToolCallData(const ToolCall& call, int64_t turn, int64_t step) {
  // A digest, never the arguments: the journal deliberately keeps values out,
  // and a fingerprint is enough to see that the same call was made twice.
  return {{"turn", turn},
          {"step", step},
          {"call_id", call.id},
          {"response_id", call.response_id},
          {"occurrence_id", call.occurrence_id},
          {"detail_id", call.detail_id},
          {"name", call.name},
          {"arguments", call.args},
          {"arguments_digest", TruncatedHash(call.args, kDigestChars)}};
}

void CancelCall(CallTask& task) {
  bool steered = SteeringState().Requested();
  task.result =
      ToolCancelled(steered ? "cancelled by steering" : "cancelled by user");
  task.trace_status = steered ? "steered" : "cancelled";
}

void ExecuteCall(CallTask& task, const ToolCall& call, int64_t turn,
                 int64_t step, const ToolContext& context,
                 int64_t global_timeout_s) {
  auto started = std::chrono::steady_clock::now();
  task.started = true;
  if (SteeringState().Requested() || AbortRequested()) {
    CancelCall(task);
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
  call_context.call_id = call.id;
  call_context.approved = task.approved;
  json start = ToolCallData(call, turn, step);
  start["activity"] = task.activity;
  call_context.progress = [start](const std::string& label) {
    json update = start;
    update["activity"]["status_label"] = label;
    Emit(Event{EventId::kToolStarted, std::move(update)});
  };
  Emit(Event{EventId::kToolStarted, std::move(start)});
  // Decided again now that the calls before it in the batch have run: one of
  // them can have made its path a link to something only a person may touch.
  task.result = RequiredApproval(*task.tool, task.args) > task.required ||
                        (!task.outside && task.tool->needs_approval &&
                         task.tool->needs_approval(task.args))
                    ? ToolFailure(ToolErrorCode::kPermissionDenied,
                                  "what this call reaches changed after it was "
                                  "approved; ask for it again")
                    : task.tool->run(task.args, call_context);
  const int64_t limit = ResultCharLimit(task);
  task.result.output = LimitOutput(std::move(task.result.output),
                                   limit < 0 ? ToolResultCap() : limit);
  // A hint pulled to its bound is still a reduced request, said where no cap
  // cuts it.
  for (const std::string& one : task.clamped) {
    AddHint(task.result, "[clamped " + one + "]");
  }
  if (SteeringState().Requested()) {
    task.result.status = CompletionStatus::kCancelled;
    task.result.error = ToolErrorCode::kNone;
    task.trace_status = "steered";
  } else {
    task.trace_status = task.result.Ok() ? "ok" : "error";
  }
  task.duration_ms = ElapsedMs(started);
  EmitToolResultObservation(task, call, turn, step);
}

std::string OccurrenceId(const std::string& response_id,
                         const std::string& call_id) {
  if (response_id.empty()) return call_id;
  return response_id + ":" + TruncatedHash(call_id, kOccurrenceChars);
}

std::string DetailId(const std::string& response_id,
                     const std::string& call_id) {
  if (response_id.empty()) return "t-" + call_id;
  return "t-" + TruncatedHash(response_id + "\n" + call_id, kDetailChars);
}

}  // namespace uagent
