// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_SESSION_VIEW_H_
#define UAGENT_INCLUDE_AGENT_SESSION_VIEW_H_
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
// A coordinator's user message without the arrival stamp (and silence line)
// written for its model.
std::string StripArrivalStamp(std::string text);
// A user message's text without the "Attached:" path trailer the model sees.
std::string StripAttachedTrailer(const std::string& text);
// A tool result without the hints written for the model only: a leading
// "[running] activity …" / "[started] subagent …" / "[detached] pid …" line
// and a subagent's final "[collaborator …]" resume line. Rows link to that
// work through their parts instead.
std::string StripModelHints(std::string text);
// Upserts one block by id and returns it (nullptr once evicted from the
// bounded view).
const json* MergeDisplayBlock(json& view, const json& block);
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
json ReadPrivateArtifact(const std::string& path, size_t offset);
json FindHttpExchange(const json& value, const std::string& id);
}  // namespace uagent
#endif  // UAGENT_INCLUDE_AGENT_SESSION_VIEW_H_
