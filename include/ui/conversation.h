// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_CONVERSATION_H_
#define UAGENT_INCLUDE_UI_CONVERSATION_H_

#include <vector>

#include "include/agent/conversation.h"
#include "include/tools/tool.h"

namespace uagent {

void PrintConversationHistory(const Conversation& conversation,
                              const std::vector<Tool>& tools);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_CONVERSATION_H_
