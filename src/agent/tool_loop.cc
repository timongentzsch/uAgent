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
#include "include/agent/session_view.h"
#include "include/agent/tool_presentation.h"
#include "include/core/activity.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/core/tool_activity.h"

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

std::string NormalizedOperation(const json& arguments) {
  if (!arguments.is_object()) return {};
  for (const char* field : {"operation", "action"}) {
    auto value = arguments.find(field);
    if (value != arguments.end() && value->is_string()) {
      return AsciiLower(Trim(value->get<std::string>()));
    }
  }
  return {};
}

}  // namespace

void Agent::PushToolResultMessage(const ToolCall& call, json message) {
  conversation_.Push(std::move(message), MessageKind::kToolResult);
  conversation_.RecordDisplay(conversation_.LastDisplayId(),
                              {{"call_id", call.id},
                               {"response_id", call.response_id},
                               {"occurrence_id", call.occurrence_id},
                               {"detail_id", call.detail_id}});
}

void Agent::AppendToolResult(const ToolCall& call, const std::string& result,
                             const ToolResult& original, double duration_ms) {
  conversation_.RecordToolDisplay(call.id,
                                  original.Ok() ? original.display : "");
  conversation_.AddStatistics({{"tool_results", 1}, {"tool_ms", duration_ms}});
  json facts = {
      {"name", call.name},
      {"status", CompletionStatusName(original.status)},
      {"duration_ms", duration_ms},
      {"output", Utf8Trunc(StripModelHints(original.output), kPreviewChars)},
      {"truncated", original.output.size() > kPreviewChars},
      {"change", Utf8Trunc(original.display, kChangePreviewChars)}};
  if (retain_exchanges_) {
    json exchange = {
        {"request", {{"name", call.name}, {"arguments", call.args}}},
        {"response", original.output},
        {"status", CompletionStatusName(original.status)},
        {"complete", true}};
    std::string body = JsonDump(exchange);
    CreatePrivateDirectories(UagentDir(kArtifactsDir));
    ScopedTempFile file(UagentDir(kArtifactsDir) + "/exchange-XXXXXX");
    if (file && WriteFully(file.Get(), body)) {
      facts["exchange_path"] = file.Release();
    }
  }
  if (original.artifact) facts["artifact"] = original.artifact->path;
  if (original.parts.is_array()) facts["parts"] = original.parts;
  facts["call_id"] = call.id;
  facts["response_id"] = call.response_id;
  facts["occurrence_id"] = call.occurrence_id;
  facts["detail_id"] = call.detail_id;
  conversation_.RecordDisplay(call.detail_id, std::move(facts));
  const Tool* tool = FindTool(tools_, call.name);
  if (tool && tool->dedupe_output && result.size() >= kToolDedupeMinChars &&
      conversation_.HasRecentToolResult(call.name, call.args, result)) {
    constexpr char kDuplicate[] =
        "[unchanged duplicate; prior read result remains in recent context]";
    PushToolResultMessage(
        call,
        {{"role", "tool"}, {"tool_call_id", call.id}, {"content", kDuplicate}});
    PublishMessage();
    DebugLog("tool_result_deduplicated",
             {{"turn", turn_id_},
              {"name", call.name},
              {"original_chars", result.size()},
              {"model_chars", sizeof(kDuplicate) - 1}});
    return;
  }
  json message = {
      {"role", "tool"}, {"tool_call_id", call.id}, {"content", result}};
  if (original.Ok() && original.read_range && result == original.output) {
    const ReadRange& range = *original.read_range;
    message[kReadRangeField] = {range.path, range.first, range.last};
  }
  PushToolResultMessage(call, std::move(message));
  PublishMessage();
}

bool Agent::RunCalls(
    const std::vector<ToolCall>& calls, int64_t& tool_count,
    std::unordered_map<std::string, int64_t>& tool_counts,
    std::unordered_map<std::string, std::string>& stable_arguments,
    int64_t step, std::chrono::steady_clock::time_point deadline,
    int64_t& consecutive_failed_tools, std::vector<ToolRejection>& rejections,
    std::vector<ActivityPollResult>& activity_polls) {
  std::vector<CallTask> tasks(calls.size());
  rejections.clear();
  activity_polls.clear();
  auto reject = [](CallTask& task, ToolErrorCode code, std::string message,
                   const char* status,
                   std::optional<ToolArgumentIssue> issue = std::nullopt) {
    task.result = ToolFailure(code, std::move(message));
    task.trace_status = status;
    task.issue = std::move(issue);
  };
  auto issue_message = [](const ToolArgumentIssue& issue) {
    return issue.message.starts_with("error:") ? issue.message
                                               : "error: " + issue.message;
  };
  for (size_t index = 0; index < calls.size(); ++index) {
    const ToolCall& call = calls[index];
    CallTask& task = tasks[index];
    if (calls.size() > 1) {
      task.ordinal = "[" + std::to_string(index + 1) + "] ";
    }
    task.raw_args = json::parse(call.args, nullptr, false);
    task.args = task.raw_args;
    task.tool = FindTool(tools_, call.name);
    const Tool* tool = task.tool;
    std::string description;
    if (tool && tool->declared_intent && task.args.is_object()) {
      // Display metadata never reaches validation, permission decisions or
      // execution. The original tool call remains intact for exact replay.
      description = ActivityLabel(JsonValue(task.args, "description", ""));
      task.args.erase("description");
    }
    if (tool) CanonicalizeToolArguments(*tool, task.args, &task.clamped);
    const json& arguments = task.args;
    bool valid = false;
    if (arguments.is_discarded() || !arguments.is_object()) {
      ToolArgumentIssue issue = ArgumentIssue(
          "arguments.malformed", "malformed tool arguments (not valid JSON)");
      reject(task, ToolErrorCode::kInvalidArguments, "error: " + issue.message,
             "malformed_arguments", issue);
    } else if (!tool) {
      ToolArgumentIssue issue =
          ArgumentIssue("tool.unknown", "unknown tool " + call.name);
      reject(task, ToolErrorCode::kNotFound, "error: unknown tool " + call.name,
             "unknown_tool", issue);
    } else if (!tool_selection_.Enabled(*tool)) {
      ToolArgumentIssue issue =
          ArgumentIssue("tool.inactive", "inactive tool " + call.name);
      reject(task, ToolErrorCode::kUnavailable,
             "error: tool is inactive for this conversation: " + call.name,
             "inactive_tool", issue);
    } else if (auto issue = FindToolArgumentIssue(*tool, arguments)) {
      std::string message = "error: invalid tool argument: " + issue->message;
      reject(task, ToolErrorCode::kInvalidArguments, std::move(message),
             "invalid_argument", std::move(issue));
    } else if (tool->validate) {
      auto semantic_issue = tool->validate(arguments);
      if (semantic_issue) {
        std::string message = issue_message(*semantic_issue);
        reject(task, ToolErrorCode::kInvalidArguments, std::move(message),
               "rejected", std::move(semantic_issue));
      } else {
        valid = true;
      }
    } else {
      valid = true;
    }
    if (valid) {
      std::string stable =
          StableArgumentError(*tool, arguments, stable_arguments);
      if (!stable.empty()) {
        ToolArgumentIssue issue =
            ArgumentIssue("arguments.unstable", stable, tool->stable_argument);
        reject(task, ToolErrorCode::kInvalidArguments, std::move(stable),
               "unstable_argument", issue);
        valid = false;
      }
    }
    if (valid && tool->max_calls_per_turn >= 0 &&
        tool_counts[call.name] >= tool->max_calls_per_turn) {
      std::string message =
          "error: " + call.name + " reached its per-turn call limit (" +
          std::to_string(tool->max_calls_per_turn) +
          "); continue from the results you have — do not reimplement it "
          "with run";
      ToolArgumentIssue issue = ArgumentIssue("tool.call_limit", message);
      reject(task, ToolErrorCode::kLimitExceeded, std::move(message),
             "call_limit", issue);
      valid = false;
    }
    if (valid) task.label = ToolSummary(*tool, arguments);
    if (!valid) {
      const json& shown =
          task.raw_args.is_discarded() ? task.args : task.raw_args;
      task.label = tool && tool->redact_invalid_arguments
                       ? ToolSummary(*tool, arguments)
                   : shown.is_discarded() ? call.args
                                          : JsonDump(shown);
    }
    const ApprovalClass required =
        valid ? RequiredApproval(*tool, arguments) : ApprovalClass::kNone;
    task.activity = {
        {"id", call.id},
        {"category", valid ? ToolActivityCategory(*tool, arguments) : "run"},
        {"label", task.label},
        {"groupable", valid &&
                          (required == ApprovalClass::kNone ||
                           (required == ApprovalClass::kYoloEligibleMutation &&
                            ApprovalIsYolo())) &&
                          call.name != "skill"}};
    task.activity["response_id"] = call.response_id;
    task.activity["call_id"] = call.id;
    task.activity["occurrence_id"] = call.occurrence_id;
    task.activity["detail_id"] = call.detail_id;
    task.activity["status_label"] =
        valid && !description.empty() ? description
        : tool ? ActivityLabel(ToolTitle(*tool) + " · " + task.label)
               : ActivityLabel(call.name);
    task.activity["label_source"] =
        valid && !description.empty() ? "model_intent" : "tool";
    conversation_.RecordDisplay(call.detail_id, {{"activity", task.activity}});
    Event call_event{EventId::kToolCall, ToolCallData(call, turn_id_, step)};
    call_event.data["activity"] = task.activity;
    if (task.issue) {
      call_event.data["issue_code"] = task.issue->code;
      call_event.data["issue_field"] = task.issue->field;
    }
    call_event.presentation = ToolCallPresentation(task, call);
    if (call_event.presentation) {
      call_event.data["view"] = call_event.presentation->view;
      // --resume replays the row from facts: same title/summary/flags the
      // live printer saw, so history matches execution exactly.
      conversation_.RecordDisplay(
          call.detail_id,
          {{"call_replay", ToolReplayJson(*call_event.presentation)}});
    }
    Emit(std::move(call_event));
    if (valid) {
      if (required == ApprovalClass::kNone ||
          approve_(*tool, arguments, turn_id_)) {
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
    if (!task.execute) {
      EmitToolResultObservation(task, call, turn_id_, step);
    }
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

  ToolContext context{deadline};
  context.turn_id = turn_id_;
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
    EmitToolResultObservation(task, calls[index], turn_id_, step);
  }

  bool cancelled = AbortRequested() && !SteeringState().Requested();
  if (!SteeringState().Requested()) ClearAbort();
  std::vector<std::string> model_results = ModelFacingToolResults(tasks);
  std::vector<json> activities;
  activities.reserve(tasks.size());
  for (const CallTask& task : tasks) {
    json activity = task.activity;
    activity["status"] = CompletionStatusName(task.result.status);
    // This receipt comes from the operation, never from shell intent.
    if (task.result.Ok() && !task.result.display.empty()) {
      activity["label"] = FirstLine(task.result.display);
      activity["groupable"] = false;
    }
    activities.push_back(std::move(activity));
  }
  GroupToolActivities(activities);
  for (size_t index = 0; index < tasks.size(); ++index) {
    tasks[index].activity = std::move(activities[index]);
    conversation_.RecordDisplay(calls[index].detail_id,
                                {{"activity", tasks[index].activity}});
    {
      Event result_event{EventId::kPresentation};
      result_event.presentation = ToolResultPresentation(
          tasks[index], calls[index], model_results[index]);
      if (result_event.presentation) {
        conversation_.RecordDisplay(
            calls[index].detail_id,
            {{"result_replay", ToolReplayJson(*result_event.presentation)}});
      }
      Emit(std::move(result_event));
    }
  }
  size_t original_chars = 0;
  size_t model_chars = 0;
  for (size_t index = 0; index < tasks.size(); ++index) {
    const ToolCall& call = calls[index];
    CallTask& task = tasks[index];
    original_chars = SaturatingAdd(original_chars, task.result.output.size());
    model_chars = SaturatingAdd(model_chars, model_results[index].size());
    AppendToolResult(call, model_results[index], task.result, task.duration_ms);
  }
  bool any_succeeded =
      std::any_of(tasks.begin(), tasks.end(),
                  [](const CallTask& task) { return task.result.Ok(); });
  int64_t failed =
      std::count_if(tasks.begin(), tasks.end(),
                    [](const CallTask& task) { return !task.result.Ok(); });
  consecutive_failed_tools =
      any_succeeded ? 0 : consecutive_failed_tools + failed;
  for (size_t index = 0; index < tasks.size(); ++index) {
    const CallTask& task = tasks[index];
    if (calls[index].name == "activity" && task.args.is_object() &&
        JsonValue(task.args, "operation", "") == "poll") {
      int64_t id = JsonValue(task.args, "id", int64_t{0});
      if (id > 0) {
        activity_polls.push_back({id, task.result.Ok(), task.result.no_change,
                                  task.result.activity_terminal});
      }
    }
    if (!task.issue) continue;
    rejections.push_back({calls[index].name, task.issue->code,
                          task.issue->field, NormalizedOperation(task.args)});
  }
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
