// Copyright 2026 Timon Gentzsch

#include "include/agent/session_search.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/transcript.h"
#include "include/core/fs.h"
#include "include/core/strings.h"

namespace uagent {
namespace {

// Parsing a session costs far more than reading it, so a file whose bytes
// cannot hold the query is skipped unparsed. Saved text is JSON-escaped only
// for quotes, backslashes and control characters; a query with one of those
// reads every file. A query split across two text parts of one message is
// not found this way.
bool MayContain(const std::string& path, const std::string& needle) {
  if (std::ranges::any_of(needle, [](unsigned char c) {
        return c == '"' || c == '\\' || c < 0x20;
      })) {
    return true;
  }
  std::string bytes, error;
  if (!ReadRegularFile(path, kSessionReadBytes, bytes, error)) return false;
  return ContainsCaseInsensitive(bytes, needle);
}

}  // namespace

std::optional<Conversation> LoadConversation(const std::string& path,
                                             std::string& error) {
  SessionLoadResult loaded = SessionStore::Inspect(path);
  if (!loaded.status.Ok() || !loaded.record) {
    error = loaded.status.message;
    return std::nullopt;
  }
  Conversation conversation;
  if (!std::move(loaded.record->state).RestoreConversation(conversation)) {
    error = "session conversation state is invalid";
    return std::nullopt;
  }
  return conversation;
}

json SearchSessions(const std::vector<SessionInfo>& sessions,
                    const std::string& query, SessionSearchLimits limits) {
  json hits = json::array();
  const std::string needle = AsciiLower(query);
  size_t scanned = 0;
  for (const SessionInfo& info : sessions) {
    if (scanned >= limits.sessions || hits.size() >= limits.hits) break;
    ++scanned;
    if (!MayContain(info.path, needle)) continue;
    std::string error;
    const auto conversation = LoadConversation(info.path, error);
    if (!conversation) continue;
    json found = TranscriptView(*conversation).Search(query, SIZE_MAX);
    const size_t keep =
        std::min({found.size(), limits.per_session, limits.hits - hits.size()});
    const std::string id = SessionIdFor(info.path);
    for (size_t i = found.size() - keep; i < found.size(); ++i) {
      json hit = std::move(found[i]);
      hit["session_id"] = id;
      hit["title"] = info.title;
      // All the session's rows that hold the words, not only those listed.
      hit["rows"] = found.size();
      hits.push_back(std::move(hit));
    }
  }
  return {{"hits", std::move(hits)},
          {"scanned", scanned},
          {"truncated", scanned < sessions.size()}};
}

}  // namespace uagent
