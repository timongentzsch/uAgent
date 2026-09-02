// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_PROTOCOL_H_
#define UAGENT_INCLUDE_AGENT_PROTOCOL_H_
// Shaping what a model response and a tool result may be. Recognizing markup
// this harness does not execute lives in tool_protocol.h; prompt authoring
// lives in prompt.h.

#include <cstdint>
#include <string>
#include <utility>

#include "include/agent/tool_protocol.h"
#include "include/api/types.h"
#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/strings.h"

namespace uagent {

// A response that is usable as evidence rather than as a step: real text, and
// no tool call in any protocol. Required of the summarizer and of the vision
// model, neither of which has tools.
inline bool ProseOnlyResponse(const ChatResult& result) {
  return !result.content.empty() && result.tool_calls.empty() &&
         !ContainsForeignToolCallMarkup(result.content);
}

// cap huge results, keeping head + tail (errors usually live at the end)
inline std::string CapResult(std::string s, int64_t cap = -1) {
  if (cap < 0) cap = ToolResultCap();
  if (cap <= 0 || static_cast<int64_t>(s.size()) <= cap) return s;
  size_t limit = static_cast<size_t>(cap);
  if (limit <= 3) return std::string(limit, '.');

  size_t omitted = s.size() - limit;
  for (int attempt = 0; attempt < 3; ++attempt) {
    std::string marker =
        "\n... [" + std::to_string(omitted) + " bytes truncated] ...\n";
    if (marker.size() >= limit) {
      return Utf8Prefix(std::move(s), limit - 3) + "...";
    }
    size_t keep = limit - marker.size();
    size_t head_end = Utf8BoundaryBefore(s, keep / 2);
    size_t tail_start = Utf8BoundaryAfter(s, s.size() - (keep - keep / 2));
    if (tail_start < head_end) tail_start = head_end;
    size_t actual_omitted = tail_start - head_end;
    if (actual_omitted == omitted) {
      return s.substr(0, head_end) + marker + s.substr(tail_start);
    }
    omitted = actual_omitted;
  }
  return Utf8Prefix(std::move(s), limit - 3) + "...";
}

inline json HarnessMessage(std::string content) {
  return {{"role", "system"}, {"content", std::move(content)}};
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_PROTOCOL_H_
