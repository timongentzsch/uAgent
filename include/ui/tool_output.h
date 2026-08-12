// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_TOOL_OUTPUT_H_
#define UAGENT_INCLUDE_UI_TOOL_OUTPUT_H_
// One terminal renderer for every tool call and result. Compact mode stays on
// one bounded line; verbose mode uses normal terminal scrollback.

#include <algorithm>
#include <cstdio>
#include <sstream>
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
  bool full_label = verbose || call.name == "run";
  if (!task.label.empty()) {
    if (full_label && task.label.find('\n') != std::string::npos) {
      prefix += '\n' + TerminalSafe(task.label);
    } else {
      std::string summary =
          full_label ? TerminalSafe(task.label)
                     : TerminalSummary(task.label, prefix.size() + 3);
      prefix += '(' + summary + ')';
    }
  }
  printf("%s%s%s\n", CYAN(), prefix.c_str(), RST());
}

inline void PrintToolDisplay(const std::string& display) {
  std::istringstream input(display);
  std::string line;
  if (!std::getline(input, line)) return;
  printf("%s•%s %s%s%s\n", DIM(), RST(), BOLD(), TerminalSafe(line).c_str(),
         RST());
  while (std::getline(input, line)) {
    const char* style = DIM();
    if (!line.empty() && line[0] == '+') style = GREEN();
    if (!line.empty() && line[0] == '-') style = RED();
    if (!line.empty() && line[0] == '@') line = "@@ " + line.substr(1);
    printf("%s    %s%s\n", style, TerminalSafe(line).c_str(), RST());
  }
}

inline void PrintToolResult(const CallTask& task, const ToolCall& call,
                            const std::string& model_output, bool verbose) {
  if (g_tty && task.result.Ok() && !task.result.display.empty()) {
    PrintToolDisplay(task.result.display);
    return;
  }
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

inline void PrintStoredToolResult(const std::string& name,
                                  const std::string& output) {
  std::string prefix = "  ← " + TerminalSafe(name.empty() ? "tool" : name);
  std::string summary =
      ToolResultSummary(ToolResult{}, output, /*truncated=*/false);
  printf("%s%s: %s%s\n", DIM(), prefix.c_str(),
         TerminalSummary(summary, prefix.size() + 2).c_str(), RST());
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_TOOL_OUTPUT_H_
