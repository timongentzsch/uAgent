// Copyright 2026 Timon Gentzsch

#include "include/agent/tool_presentation.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "include/agent/protocol.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/strings.h"

namespace uagent {

bool IsActivityPoll(const CallTask& task) {
  return task.tool && task.tool->name == "activity" &&
         JsonValue(task.args, "operation", "") == "poll" &&
         JsonValue(task.args, "id", int64_t{0}) > 0;
}

size_t TextLines(const std::string& text) {
  if (text.empty()) return 0;
  return static_cast<size_t>(std::count(text.begin(), text.end(), '\n')) +
         (text.back() == '\n' ? 0 : 1);
}

std::string ToolResultSummary(const ToolResult& result,
                              const std::string& output, bool truncated) {
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

PresentationRecord ToolCallPresentation(const CallTask& task,
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

PresentationRecord ToolCallPresentation(const std::string& name,
                                        const json& arguments,
                                        const std::vector<Tool>& tools,
                                        const std::string& ordinal) {
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

PresentationRecord ToolResultPresentation(const CallTask& task,
                                          const ToolCall& call,
                                          const std::string& model_output,
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

json ToolReplayJson(const PresentationRecord& record) {
  json value = {{"title", record.title},
                {"summary", record.summary},
                {"poll", record.poll}};
  if (record.multiline) {
    value["multiline"] = true;
    value["detail"] = Utf8Trunc(record.detail, kReplayDetailChars);
  }
  return value;
}

PresentationRecord StoredToolResultPresentation(const std::string& name,
                                                const std::string& output,
                                                const std::string& display,
                                                PresentationStatus status) {
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
  record.summary =
      TerminalSummary(ToolResultSummary(replayed, output,
                                        /*truncated=*/false),
                      record.title.size() + kRecordTitleReserveChars);
  return record;
}

namespace {

struct PollAnchor {
  std::chrono::steady_clock::time_point started;
  std::chrono::steady_clock::time_point seen;
};

// An activity can also vanish without a final poll, by completing in the
// background or being stopped, so anchors expire instead of relying on every
// such path to announce itself.
constexpr std::chrono::hours kPollAnchorTtl{1};

std::unordered_map<int64_t, PollAnchor>& PollAnchors() {
  static std::unordered_map<int64_t, PollAnchor> anchors;
  return anchors;
}

}  // namespace

std::chrono::steady_clock::duration PollElapsed(int64_t activity_id) {
  auto now = std::chrono::steady_clock::now();
  auto& anchors = PollAnchors();
  auto [it, inserted] = anchors.try_emplace(activity_id, PollAnchor{now, now});
  it->second.seen = now;
  auto elapsed = now - it->second.started;
  if (inserted) {
    std::erase_if(anchors, [now](const auto& entry) {
      return entry.second.seen + kPollAnchorTtl < now;
    });
  }
  return elapsed;
}

void ClearPollAnchor(int64_t activity_id) { PollAnchors().erase(activity_id); }

}  // namespace uagent
