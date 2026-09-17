// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_DISPATCH_H_
#define UAGENT_INCLUDE_AGENT_DISPATCH_H_
// One tool call in flight: the state the loop carries for it, how its
// result is traced, and the guarded execution itself.

#include <algorithm>
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
  json activity = json::object();
  json raw_args;
  json args;
  std::vector<std::string> clamped;  // pacing hints pulled to their bound
  ToolResult result;
  std::optional<ToolArgumentIssue> issue;
  std::string trace_status;
  std::string label, ordinal;
  double duration_ms = 0;
  bool execute = false;
  bool started = false;
};

int64_t ResultCharLimit(const CallTask& task);

// Keep recovery metadata at the tail even when a result must be shortened.
// The artifact itself stays out of context; only this small locator rides with
// the result.
std::string ModelResultText(const ToolResult& result, int64_t cap);

// Preserve small results and divide the remaining model-facing budget evenly
// across larger siblings. The original per-tool-capped strings remain intact
// for diagnostics and terminal output.
std::vector<std::string> ModelFacingToolResults(
    const std::vector<CallTask>& tasks, int64_t budget = -1);

json ToolResultData(const CallTask& task, const ToolCall& call,
                           int64_t turn, int64_t step);

PresentationRecord ToolResultObservation(const CallTask& task,
                                                const ToolCall& call);

void EmitToolResultObservation(const CallTask& task, const ToolCall& call,
                                      int64_t turn, int64_t step);

json ToolCallData(const ToolCall& call, int64_t turn, int64_t step);

void CancelCall(CallTask& task);

void ExecuteCall(CallTask& task, const ToolCall& call, int64_t turn,
                        int64_t step, const ToolContext& context,
                        int64_t global_timeout_s);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_DISPATCH_H_
