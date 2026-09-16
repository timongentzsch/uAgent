// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_CONVERSATION_H_
#define UAGENT_INCLUDE_UI_CONVERSATION_H_

#include <string>
#include <vector>

#include "include/agent/conversation.h"
#include "include/core/json.h"
#include "include/tools/tool.h"

namespace uagent {

void PrintConversationHistory(const Conversation& conversation,
                              const std::vector<Tool>& tools);
void PrintModelContext(const json& request);

// Terminal rendering of an archived tool trace. The facts come from
// agent/trace.h; the agent itself never prints.
std::string PrintToolCallSummary(const json& call,
                                 const std::vector<Tool>& tools);
void PrintTraceToolCall(const json& call, const std::vector<Tool>& tools,
                        const std::string& ordinal);
void PrintTraceToolResult(const json& call, const std::string& ordinal);
void PrintLatestTrace(const json& archive, const std::vector<Tool>& tools);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_CONVERSATION_H_
