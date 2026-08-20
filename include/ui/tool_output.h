// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_TOOL_OUTPUT_H_
#define UAGENT_INCLUDE_UI_TOOL_OUTPUT_H_
// One terminal renderer for every tool call and result. Compact mode stays on
// one bounded line; verbose mode uses normal terminal scrollback.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include "include/agent/dispatch.h"
#include "include/core/events.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/ui/presentation.h"

namespace uagent {

inline bool IsActivityPoll(const CallTask& task) {
  return task.tool && task.tool->name == "activity" &&
         !(task.tool->mutates && task.tool->mutates(task.args)) &&
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
  if (lines > 1) {
    summary += " … · " + std::to_string(lines) + " lines · " +
               FmtCount(static_cast<int64_t>(output.size())) + " chars";
  }
  if (truncated) summary += " · truncated";
  if (!result.Ok()) {
    summary = std::string(CompletionStatusName(result.status)) + ": " + summary;
  }
  return summary;
}

inline PresentationRecord ToolCallPresentation(const CallTask& task,
                                               const ToolCall& call,
                                               bool verbose) {
  PresentationRecord record;
  record.kind = PresentationKind::kToolCall;
  record.id = call.id;
  record.title = task.ordinal + call.name;
  record.skill = task.tool && task.tool->name == "skill";
  record.poll =
      IsActivityPoll(task);  // outcome unknown until result; see below
  bool full_label = verbose || (task.tool && task.tool->verbatim_label);
  record.verbatim = full_label;
  if (!task.label.empty()) {
    if (full_label && task.label.find('\n') != std::string::npos) {
      record.detail = task.label;
      record.multiline = true;
    } else {
      record.summary = Utf8Trunc(task.label, size_t{2048});
    }
  }
  return record;
}

inline PresentationRecord ToolResultPresentation(
    const CallTask& task, const ToolCall& call, const std::string& model_output,
    bool verbose) {
  PresentationRecord record;
  record.kind = PresentationKind::kToolResult;
  record.id = call.id;
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
  if (g_tty && task.result.Ok() && !task.result.display.empty()) {
    record.detail = task.result.display;
    record.multiline = true;
    record.change_display = true;
    return record;
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
    std::string prefix = "  ← " + record.title;
    record.summary = verbose ? std::move(summary)
                             : TerminalSummary(summary, prefix.size() + 2);
  }
  return record;
}

inline void PrintStoredToolResult(const std::string& name,
                                  const std::string& output) {
  PresentationRecord record;
  record.kind = PresentationKind::kToolResult;
  record.status = PresentationStatus::kSucceeded;
  record.title = name.empty() ? "tool" : name;
  record.summary = TerminalSummary(ToolResultSummary(ToolResult{}, output,
                                                     /*truncated=*/false),
                                   record.title.size() + 6);
  PrintPresentation(record);
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_TOOL_OUTPUT_H_
