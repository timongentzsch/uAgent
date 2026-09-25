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

// Mirrors the web client's stripAttachedTrailer: stored user text keeps the
// "Attached:" path trailer for the model payload, but transcripts render
// the delivery gallery below instead of leaking host paths.
// One dim "name · delivery" row per recorded attachment delivery, mirroring
// the web message gallery. Empty when there is nothing to show.
std::string AttachmentDeliveryRows(const json& deliveries);
void PrintModelContext(const json& request);

// Terminal line for a stored tool call; the agent itself never prints.
std::string PrintToolCallSummary(const json& call,
                                 const std::vector<Tool>& tools);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_CONVERSATION_H_
