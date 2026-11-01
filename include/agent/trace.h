// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_TRACE_H_
#define UAGENT_INCLUDE_AGENT_TRACE_H_
// Shared state and data for live, resumed, and archived traces. Terminal
// rendering of this data lives in ui/presentation.h; the agent only
// supplies facts.

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/conversation.h"
#include "include/agent/protocol.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/tools/tool.h"

namespace uagent {

inline json ParsedToolCallArguments(const json& function) {
  if (!function.is_object() || !function.contains("arguments")) {
    return json::object();
  }
  if (!function["arguments"].is_string()) return function["arguments"];
  std::string raw = function["arguments"].get<std::string>();
  json arguments = json::parse(raw, nullptr, false);
  return arguments.is_discarded() ? json(std::move(raw)) : std::move(arguments);
}

// conversation messages + their kinds -> the call/result trace array
json ToolTraceMessages(const json& messages, const json& kinds);

inline const json* LatestTraceSegment(const json& archive) {
  auto segment =
      std::find_if(archive.rbegin(), archive.rend(), [](const json& item) {
        std::string reason = JsonValue(item, "reason", "");
        return reason == "tool_trace";
      });
  return segment == archive.rend() ? nullptr : &*segment;
}

inline json LatestToolTraceJson(const json& archive) {
  const json* segment = LatestTraceSegment(archive);
  if (!segment) return json::array();
  return ToolTraceMessages(JsonValue(*segment, "messages", json::array()),
                           JsonValue(*segment, "message_kinds", json::array()));
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_TRACE_H_
