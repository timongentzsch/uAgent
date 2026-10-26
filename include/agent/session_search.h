// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_SESSION_SEARCH_H_
#define UAGENT_INCLUDE_AGENT_SESSION_SEARCH_H_
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "include/agent/conversation.h"
#include "include/agent/session_store.h"
#include "include/core/json.h"

namespace uagent {

// A saved session's conversation, or why it cannot be read.
std::optional<Conversation> LoadConversation(const std::string& path,
                                             std::string& error);

struct SessionSearchLimits {
  size_t sessions = 0;     // read at most this many, in the order given
  size_t hits = 0;         // in all
  size_t per_session = 0;  // its newest rows
};

// The rows of `sessions` that contain `query` (TranscriptView::Search), each
// with its session_id and title: {hits, scanned, truncated}. Truncated means
// a limit stopped the search before every session was read.
json SearchSessions(const std::vector<SessionInfo>& sessions,
                    const std::string& query, SessionSearchLimits limits);

}  // namespace uagent
#endif  // UAGENT_INCLUDE_AGENT_SESSION_SEARCH_H_
