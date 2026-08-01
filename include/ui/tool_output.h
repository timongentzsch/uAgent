// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_TOOL_OUTPUT_H_
#define UAGENT_INCLUDE_UI_TOOL_OUTPUT_H_
// One terminal renderer for every tool call and result. Compact mode stays on
// one bounded line; verbose mode uses normal terminal scrollback.

#include <algorithm>
#include <cstdio>
#include <string>

#include "include/agent/dispatch.h"
#include "include/core/strings.h"
#include "include/core/term.h"

namespace uagent {

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

inline std::string ToolOutputForDisplay(const CallTask& task,
                                        const std::string& model_output,
                                        bool verbose) {
  return verbose ? ModelResultText(task.result, ResultCharLimit(task))
                 : model_output;
}

inline void PrintToolCall(const CallTask& task, const ToolCall& call,
                          bool verbose) {
  std::string prefix = "→ " + task.ordinal + TerminalSafe(call.name);
  if (task.label.empty()) {
    printf("%s%s%s\n", CYAN(), prefix.c_str(), RST());
  } else if (verbose && task.label.find('\n') != std::string::npos) {
    printf("%s%s%s\n%s\n", CYAN(), prefix.c_str(), RST(),
           TerminalSafe(task.label).c_str());
  } else {
    std::string summary = verbose
                              ? TerminalSafe(task.label)
                              : TerminalSummary(task.label, prefix.size() + 3);
    printf("%s%s(%s)%s\n", CYAN(), prefix.c_str(), summary.c_str(), RST());
  }
}

inline void PrintToolResult(const CallTask& task, const ToolCall& call,
                            const std::string& model_output, bool verbose) {
  const char* style = DIM();
  if (task.result.status == CompletionStatus::kFailed ||
      task.result.status == CompletionStatus::kTimedOut) {
    style = RED();
  } else if (task.result.status == CompletionStatus::kCancelled) {
    style = YEL();
  }
  std::string prefix = "  ← " + task.ordinal + TerminalSafe(call.name);
  std::string shown = ToolOutputForDisplay(task, model_output, verbose);
  if (verbose && shown.find('\n') != std::string::npos) {
    std::string status =
        task.result.Ok()
            ? ""
            : " " + std::string(CompletionStatusName(task.result.status));
    printf("%s%s%s%s\n%s\n", style, prefix.c_str(), status.c_str(), RST(),
           TerminalSafe(shown).c_str());
    return;
  }
  bool truncated = !verbose && model_output.size() < task.result.output.size();
  std::string output;
  if (verbose && !shown.empty()) {
    output = TerminalSafe(shown);
  } else {
    std::string summary = ToolResultSummary(task.result, shown, truncated);
    output = verbose ? TerminalSafe(summary)
                     : TerminalSummary(summary, prefix.size() + 2);
  }
  printf("%s%s: %s%s\n", style, prefix.c_str(), output.c_str(), RST());
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_TOOL_OUTPUT_H_
