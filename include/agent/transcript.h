// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_TRANSCRIPT_H_
#define UAGENT_INCLUDE_AGENT_TRANSCRIPT_H_
// A conversation's transcript as nodes (node.h): built from its records, held
// as a bounded view one reducer patches, and paged, searched and looked up.

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "include/agent/conversation.h"
#include "include/core/json.h"
#include "include/core/limits.h"
namespace uagent {
// A view or page holds at most this many nodes and bytes.
inline constexpr size_t kViewNodes = 64;
inline constexpr size_t kViewBytes = KiB(384);

struct TranscriptEntry {
  const json* message;
  std::string kind;
};

// Oldest first; with `calls`, an assistant message's calls too.
void EntryNodes(const Conversation& conversation, uint64_t sequence,
                const TranscriptEntry& entry, bool calls,
                std::vector<json>& out);
// A record as a person reads it: no model hints, no "Attached:" trailer.
std::string EntryText(const json& facts, uint64_t sequence, const json& message,
                      bool reasoning = false);
// The last message's nodes as published; its calls arrive as they run.
std::vector<json> LastNodes(const Conversation& conversation);

// The borrowed conversation must outlive it, unchanged after the first query.
class TranscriptView {
 public:
  explicit TranscriptView(const Conversation& conversation);
  // The nodes before sequence `before` (0: the latest), ending on whole turns.
  json Page(uint64_t before = 0) const;
  // A node's text in full, by its id, a page from `offset`.
  json Detail(const std::string& id, size_t offset = 0) const;
  // A call's exact retained arguments and result, by its node id.
  json Exchange(const std::string& id, size_t offset = 0) const;
  // Nodes whose text contains `query`, ignoring ASCII case, oldest first.
  json Search(const std::string& query, size_t limit) const;

 private:
  const Conversation& conversation_;
  const std::map<uint64_t, TranscriptEntry>& Index() const;
  mutable std::once_flag indexed_;
  mutable std::map<uint64_t, TranscriptEntry> entries_;
};

// Presentation only: no system prompt, provider replay, base64 or path link.
json ConversationView(const Conversation& conversation, uint64_t before = 0);
// A user message's text without the "Attached:" path trailer the model sees.
std::string StripAttachedTrailer(const std::string& text);
// Returns how many of the oldest nodes went; `patches` receives the changes.
size_t MergeNodes(json& view, const json& nodes, json* patches = nullptr);
// The one reducer of a session's view; false means the event is live-only.
bool ApplySessionEvent(json& state, const std::string& type, const json& data,
                       json* patches = nullptr);
// One page of `text` from `offset`, cut at UTF-8 boundaries.
json PageText(const std::string& text, size_t offset);
json FindHttpExchange(const json& value, const std::string& id);
// The HTTP exchanges of the latest model request, from the display facts.
json LatestHttp(const json& facts);
}  // namespace uagent
#endif  // UAGENT_INCLUDE_AGENT_TRANSCRIPT_H_
