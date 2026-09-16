// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_TOOL_OUTPUT_H_
#define UAGENT_INCLUDE_UI_TOOL_OUTPUT_H_
// One terminal renderer for every tool call and result. Calls are complete;
// compact result rows stay bounded, while verbose results use scrollback.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/dispatch.h"
#include "include/core/events.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/ui/presentation.h"

namespace uagent {

inline bool IsActivityPoll(const CallTask& task) {
  return task.tool && task.tool->name == "activity" &&
         JsonValue(task.args, "operation", "") == "poll" &&
         JsonValue(task.args, "id", int64_t{0}) > 0;
}

inline size_t TextLines(const std::string& text) {
  if (text.empty()) return 0;
  return static_cast<size_t>(std::count(text.begin(), text.end(), '\n')) +
         (text.back() == '\n' ? 0 : 1);
}

inline std::string ToolResultSummary(const ToolResult& result,
                                     const std::string& output,
                                     bool truncated) {
  std::string summary = output.empty() ? "(empty)" : FirstLine(output);
  size_t lines = TextLines(output);
  // The first line is shown, so the count reports what is elided: a header
  // line and a total would otherwise print two numbers for one result.
  if (lines > 1) {
    summary += AsciiGlyphs(" … · +") + std::to_string(lines - 1) +
               AsciiGlyphs(" lines · ") +
               FmtCount(static_cast<int64_t>(output.size())) + " chars";
  }
  if (truncated) summary += AsciiGlyphs(" · truncated");
  if (!result.Ok()) {
    summary = std::string(CompletionStatusName(result.status)) + ": " + summary;
  }
  return summary;
}

// Compact rows stay bounded: a long result is summarised by its first line
// and a count, and /verbose prints the whole thing.
inline PresentationRecord ToolCallPresentation(const CallTask& task,
                                               const ToolCall& call) {
  PresentationRecord record;
  record.kind = PresentationKind::kToolCall;
  record.id = call.id;
  record.activity = task.activity;
  record.title = task.ordinal + call.name;
  record.skill = task.tool && task.tool->name == "skill";
  record.poll =
      IsActivityPoll(task);  // outcome unknown until result; see below
  SetCallLabel(record, task.label);
  return record;
}

// Stored transcript path: same row as a live call, from replay data.
inline PresentationRecord ToolCallPresentation(
    const std::string& name, const json& arguments,
    const std::vector<Tool>& tools, const std::string& ordinal = "") {
  CallTask task;
  task.tool = FindTool(tools, name);
  task.ordinal = ordinal;
  task.label = task.tool && arguments.is_object()
                   ? ToolSummary(*task.tool, arguments)
                   : (arguments.is_string() ? arguments.get<std::string>()
                                            : JsonDump(arguments));
  ToolCall call;
  call.name = name;
  return ToolCallPresentation(task, call);
}

inline PresentationRecord ToolResultPresentation(
    const CallTask& task, const ToolCall& call, const std::string& model_output,
    bool verbose) {
  PresentationRecord record;
  record.kind = PresentationKind::kToolResult;
  record.id = call.id;
  record.activity = task.activity;
  record.title = task.ordinal + call.name;
  if (task.result.status == CompletionStatus::kCancelled) {
    record.status = PresentationStatus::kCancelled;
  } else if (!task.result.Ok()) {
    record.status = PresentationStatus::kFailed;
  } else {
    record.status = PresentationStatus::kSucceeded;
  }
  if (task.result.artifact) {
    record.artifacts.push_back({"tool-output", task.result.artifact->path,
                                task.result.artifact->bytes});
  }
  if (IsActivityPoll(task)) {
    int64_t activity_id = JsonValue(task.args, "id", int64_t{0});
    if (task.result.Ok() && task.result.no_change) {
      double elapsed_s =
          std::chrono::duration<double>(PollElapsed(activity_id)).count();
      record.poll = true;
      record.summary = "waited on activity " + std::to_string(activity_id) +
                       " · " + FmtDuration(elapsed_s);
      return record;
    }
    ClearPollAnchor(activity_id);
  }
  // A change is told entirely by its diff, so the receipt is the whole row.
  if (task.result.Ok() && !task.result.display.empty()) {
    record.change = task.result.display;
    if (!task.tool || !task.tool->declared_intent) return record;
  }

  std::string shown = verbose
                          ? ModelResultText(task.result, ResultCharLimit(task))
                          : model_output;
  if (verbose && shown.find('\n') != std::string::npos) {
    record.detail = shown;
    record.multiline = true;
    return record;
  }
  bool truncated = !verbose && model_output.size() < task.result.output.size();
  if (verbose && !shown.empty()) {
    record.summary = shown;
  } else {
    std::string summary = ToolResultSummary(task.result, shown, truncated);
    record.summary = std::move(summary);
  }
  return record;
}

// Kept receipts replay exactly what the live row showed. Only the compact
// row shape is recorded (title/summary/flags); bodies, diffs and groups
// already travel on the view block, so facts stay small and sessions saved
// before this change fall back to the legacy synthesis. There is no render
// flag: the worker's render bit only ever gated worker stdout (which does
// not exist); every visible CLI row is drawn client-side with render forced
// on, so presence of the fact is the show condition. Poll suppression rides
// the poll flag through the shared printer.
inline json ToolReplayJson(const PresentationRecord& record) {
  json value = {{"title", record.title},
                {"summary", record.summary},
                {"poll", record.poll}};
  if (record.multiline) {
    value["multiline"] = true;
    value["detail"] = Utf8Trunc(record.detail, 2048);
  }
  return value;
}

inline PresentationRecord StoredToolResultPresentation(
    const std::string& name, const std::string& output,
    const std::string& display = "",
    PresentationStatus status = PresentationStatus::kNeutral) {
  PresentationRecord record;
  record.kind = PresentationKind::kToolResult;
  record.status = status;
  ToolResult replayed;
  if (status == PresentationStatus::kFailed) {
    replayed.status = CompletionStatus::kFailed;
  }
  if (status == PresentationStatus::kCancelled) {
    replayed.status = CompletionStatus::kCancelled;
  }
  record.title = name.empty() ? "tool" : name;
  // A kept receipt replays as it was drawn, the way the live row showed it.
  if (status == PresentationStatus::kSucceeded && !display.empty()) {
    record.change = display;
    return record;
  }
  record.summary = TerminalSummary(ToolResultSummary(replayed, output,
                                                     /*truncated=*/false),
                                   record.title.size() + 6);
  return record;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_TOOL_OUTPUT_H_
