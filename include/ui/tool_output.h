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

// A tool that draws a change receipt says what changed in the receipt, so the
// first line of its output only restates it. What follows does not: a file
// write has nothing there, a script that was written and then run has its
// whole result there.
inline std::string OutputBelowReceipt(const std::string& output) {
  size_t newline = output.find('\n');
  if (newline == std::string::npos) return "";
  std::string body = output.substr(newline + 1);
  while (!body.empty() && body.back() == '\n') body.pop_back();
  return body;
}

// Compact rows stay bounded: a long result keeps its head and reports the rest
// by count, and /verbose prints the whole thing.
inline constexpr size_t kChangeResultLines = 12;

inline std::string BoundedLines(const std::string& text, size_t max_lines) {
  size_t at = 0;
  for (size_t line = 0; line < max_lines; ++line) {
    size_t newline = text.find('\n', at);
    if (newline == std::string::npos) return text;
    at = newline + 1;
  }
  return text.substr(0, at) + "… · " + std::to_string(TextLines(text)) +
         " lines · " + FmtCount(static_cast<int64_t>(text.size())) + " chars";
}

inline PresentationRecord ToolCallPresentation(const CallTask& task,
                                               const ToolCall& call) {
  PresentationRecord record;
  record.kind = PresentationKind::kToolCall;
  record.id = call.id;
  record.title = task.ordinal + call.name;
  record.skill = task.tool && task.tool->name == "skill";
  record.poll =
      IsActivityPoll(task);  // outcome unknown until result; see below
  SetCallLabel(record, task.label);
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
    record.change = task.result.display;
    std::string body = OutputBelowReceipt(
        verbose ? ModelResultText(task.result, ResultCharLimit(task))
                : model_output);
    if (body.empty()) return record;
    record.detail = verbose ? std::move(body)
                            : BoundedLines(body, kChangeResultLines);
    record.multiline = true;
    return record;
  }

  std::string shown = verbose
                          ? ModelResultText(task.result, ResultCharLimit(task))
                          : model_output;
  // Without a terminal there is no diff block above the row, but the receipt
  // line is still the first thing the tool printed. Summarising from it would
  // report `[script: ...]` and never the script's result.
  if (task.result.Ok() && !task.result.display.empty()) {
    std::string body = OutputBelowReceipt(shown);
    if (!body.empty()) shown = std::move(body);
  }
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

inline PresentationRecord StoredToolResultPresentation(
    const std::string& name, const std::string& output,
    const std::string& display = "") {
  PresentationRecord record;
  record.kind = PresentationKind::kToolResult;
  // A stored result keeps the text the model saw, not the status the live row
  // was coloured from. Tools write a failure with an `error: ` prefix, so
  // replay reads the signal the model read instead of resuming every call as
  // a success and rendering the whole scrollback in the success colour.
  ToolResult replayed;
  if (output.rfind("error: ", 0) == 0) {
    replayed.status = CompletionStatus::kFailed;
  }
  record.status = replayed.Ok() ? PresentationStatus::kSucceeded
                                : PresentationStatus::kFailed;
  record.title = name.empty() ? "tool" : name;
  // A kept receipt replays as it was drawn, the way the live row showed it.
  if (replayed.Ok() && !display.empty()) {
    record.change = display;
    std::string body = OutputBelowReceipt(output);
    if (!body.empty()) {
      record.detail = BoundedLines(body, kChangeResultLines);
      record.multiline = true;
    }
    return record;
  }
  record.summary = TerminalSummary(ToolResultSummary(replayed, output,
                                                     /*truncated=*/false),
                                   record.title.size() + 6);
  return record;
}

inline void PrintStoredToolResult(const std::string& name,
                                  const std::string& output,
                                  const std::string& display = "") {
  PrintPresentation(StoredToolResultPresentation(name, output, display));
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_TOOL_OUTPUT_H_
