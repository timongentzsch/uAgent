// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_PROTOCOL_H_
#define UAGENT_INCLUDE_AGENT_PROTOCOL_H_
// Shaping what a model response and a tool result may be. Recognizing markup
// this harness does not execute lives in tool_protocol.h; prompt authoring
// lives in prompt.h.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "include/api/tool_protocol.h"
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

inline json HarnessMessage(std::string content) {
  return {{"role", "system"}, {"content", std::move(content)}};
}

// Text the harness hands a model in the user role, marked as what it is:
// "[chat, not a user message] Nobody is typing now."
inline std::string HarnessNote(std::string_view kind, std::string_view text) {
  return "[" + std::string(kind) + ", not a user message] " + std::string(text);
}

// Stable tool-call identities shared by request construction and view
// projection. Widths come from limits.h so the two sites cannot drift:
// occurrence is `<response_id>:<hash16>`, detail is `t-<hash24>`.
// Empty response_id falls back to the bare call id (`t-<id>` for detail).
std::string OccurrenceId(const std::string& response_id,
                         const std::string& call_id);
std::string DetailId(const std::string& response_id,
                     const std::string& call_id);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_PROTOCOL_H_
