// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_TOOL_PRESENTATION_H_
#define UAGENT_INCLUDE_AGENT_TOOL_PRESENTATION_H_
// Observation records for tool calls and results. These builders are
// presentation-shaped but state-light and domain-owned: the agent loop
// attaches them to events and replays, and terminal/browser runners render
// the records. Nothing here touches a terminal; rendering lives in ui/.
// Bodies live in src/agent/tool_presentation.cc.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "include/agent/dispatch.h"
#include "include/core/events.h"
#include "include/tools/tool.h"

namespace uagent {

bool IsActivityPoll(const CallTask& task);

size_t TextLines(const std::string& text);

std::string ToolResultSummary(const ToolResult& result,
                              const std::string& output, bool truncated);

// A result row carries both a bounded summary (first line and a count) and
// the output itself; the client decides which one to show.
PresentationRecord ToolCallPresentation(const CallTask& task,
                                        const ToolCall& call);

// Stored transcript path: same row as a live call, from replay data.
PresentationRecord ToolCallPresentation(const std::string& name,
                                        const json& arguments,
                                        const std::vector<Tool>& tools);

PresentationRecord ToolResultPresentation(const CallTask& task,
                                          const ToolCall& call,
                                          const std::string& model_output);

// Kept receipts replay exactly what the live row showed. Only the compact
// row shape is recorded (title/summary/flags); bodies, diffs and groups
// already travel on the view block, so facts stay small and sessions saved
// before this change fall back to the legacy synthesis.
json ToolReplayJson(const PresentationRecord& record);

PresentationRecord StoredToolResultPresentation(
    const std::string& name, const std::string& output,
    const std::string& display = "",
    PresentationStatus status = PresentationStatus::kNeutral);

// Elapsed time since the first no-change poll of this activity id. Anchors
// expire instead of relying on every completion path to announce itself.
std::chrono::steady_clock::duration PollElapsed(int64_t activity_id);
void ClearPollAnchor(int64_t activity_id);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_TOOL_PRESENTATION_H_
