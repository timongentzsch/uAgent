// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_SESSION_VIEW_H_
#define UAGENT_INCLUDE_AGENT_SESSION_VIEW_H_
#include <cstdint>
#include <string>

#include "include/agent/conversation.h"
#include "include/core/json.h"
namespace uagent {
// Presentation projection only: no system prompts, opaque provider replay,
// base64 images, or arbitrary-path links. Full retained details are paged.
json ConversationView(const Conversation& conversation, uint64_t before = 0);
json LastMessageView(const Conversation& conversation);
// A user message's text without the "Attached:" path trailer the model sees.
std::string StripAttachedTrailer(const std::string& text);
// A tool result without the "[collaborator …]" line that tells the model how
// to resume a subagent; rows link to the agent through their facts instead.
std::string StripToolTrailer(std::string text);
void MergeDisplayBlock(json& view, const json& block);
// Apply an event represented in a session snapshot; false means live-only.
bool ApplySessionEvent(json& state, const std::string& type, const json& data);
json ConversationDetail(const Conversation& conversation, const std::string& id,
                        size_t offset);
// Exact retained tool arguments/result, independent of presentation previews.
json ConversationExchange(const Conversation& conversation,
                          const std::string& id, size_t offset = 0);
json ReadPrivateArtifact(const std::string& path, size_t offset);
json FindHttpExchange(const json& value, const std::string& id);
}  // namespace uagent
#endif  // UAGENT_INCLUDE_AGENT_SESSION_VIEW_H_
