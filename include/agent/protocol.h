// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_PROTOCOL_H_
#define UAGENT_INCLUDE_AGENT_PROTOCOL_H_
// What a model response and a tool result may be.

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

// Real text and no tool call in any protocol.
inline bool ProseOnlyResponse(const ChatResult& result) {
  return !result.content.empty() && result.tool_calls.empty() &&
         !ContainsForeignToolCallMarkup(result.content);
}

inline json HarnessMessage(std::string content) {
  return {{"role", "system"}, {"content", std::move(content)}};
}

// Text the harness sends in the user role, marked as not a user's.
inline std::string HarnessNote(std::string_view kind, std::string_view text) {
  return "[" + std::string(kind) + ", not a user message] " + std::string(text);
}

// `<response_id>:<hash16>` and `t-<hash24>`; the call id with no response_id.
std::string OccurrenceId(const std::string& response_id,
                         const std::string& call_id);
std::string DetailId(const std::string& response_id,
                     const std::string& call_id);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_PROTOCOL_H_
