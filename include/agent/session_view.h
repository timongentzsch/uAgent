// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_SESSION_VIEW_H_
#define UAGENT_INCLUDE_AGENT_SESSION_VIEW_H_
// A read-only view over a conversation's rows for paging, search and
// lookup. Used by the host's snapshots and the tools that read history.

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "include/agent/conversation.h"
#include "include/core/json.h"
namespace uagent {
struct TranscriptEntry {
  const json* message;
  std::string kind;
};

// Borrows a conversation that must stay alive and unchanged after the first
// query. The index is built only when paging or a fallback lookup needs it.
class TranscriptView {
 public:
  explicit TranscriptView(const Conversation& conversation);
  json Page(uint64_t before = 0) const;
  json Detail(const std::string& id, size_t offset = 0) const;
  json Exchange(const std::string& id, size_t offset = 0) const;
  // The rows a reader sees whose text contains `query`, ignoring ASCII case,
  // oldest first, at most `limit`: each its sequence, kind, how often it
  // contains the query and a snippet around the first time.
  json Search(const std::string& query, size_t limit) const;

 private:
  const Conversation& conversation_;
  const std::map<uint64_t, TranscriptEntry>& Index() const;
  mutable std::once_flag indexed_;
  mutable std::map<uint64_t, TranscriptEntry> entries_;
};

// Presentation projection only: no system prompts, opaque provider replay,
// base64 images, or arbitrary-path links. Full retained details are paged.
json ConversationView(const Conversation& conversation, uint64_t before = 0);
json LastMessageView(const Conversation& conversation);
// A user message's text without the "Attached:" path trailer the model sees.
std::string StripAttachedTrailer(const std::string& text);
// Upserts one block by id and returns it (nullptr once evicted from the
// bounded view).
const json* MergeDisplayBlock(json& view, const json& block);
// Upserts a checkpoint's blocks and bounds the view once, not per block.
// Returns how many of the oldest rows went; `changed`, when given, receives
// the rows that are new or different, in view order.
size_t MergeDisplayBlocks(json& view, const json& blocks,
                          json* changed = nullptr);
// The one reducer of a session's view: every holder of a view (worker, host,
// terminal) folds events through it. False means the event is live-only.
// `patch`, when given, receives the {kind:"block"} frame that brings another
// copy of the view to the same state, or stays null when no block changed.
bool ApplySessionEvent(json& state, const std::string& type, const json& data,
                       json* patch = nullptr);
json ConversationDetail(const Conversation& conversation, const std::string& id,
                        size_t offset);
// Exact retained tool arguments/result, independent of presentation previews.
json ConversationExchange(const Conversation& conversation,
                          const std::string& id, size_t offset = 0);
// One page of a long text from `offset`, cut at UTF-8 boundaries, with where
// the next starts and whether there is more.
json PageText(const std::string& text, size_t offset);
json FindHttpExchange(const json& value, const std::string& id);
// The HTTP exchanges of the latest model request, from the display facts.
json LatestHttp(const json& facts);
}  // namespace uagent
#endif  // UAGENT_INCLUDE_AGENT_SESSION_VIEW_H_
