// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_TRANSCRIPT_H_
#define UAGENT_INCLUDE_AGENT_TRANSCRIPT_H_
// A conversation's transcript as nodes (node.h): built from its records
// (transcript_tree.cc), held as a bounded view the one reducer patches
// (view_reducer.cc), and read in pages, searched and looked up by node id
// (transcript_query.cc).

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

// The nodes one record makes, oldest first; with `calls`, an assistant
// message's calls too.
void EntryNodes(const Conversation& conversation, uint64_t sequence,
                const TranscriptEntry& entry, bool calls,
                std::vector<json>& out);
// What a person reads of a record: a tool result's output without the
// model's hints, a message of yours without its "Attached:" trailer; with
// `reasoning`, a reply's thinking.
std::string EntryText(const json& facts, uint64_t sequence, const json& message,
                      bool reasoning = false);
// The nodes of the conversation's last message as it is published; its
// calls arrive as they run.
std::vector<json> LastNodes(const Conversation& conversation);

// Borrows a conversation that must stay alive and unchanged after the first
// query. The index is built only when a query needs it.
class TranscriptView {
 public:
  explicit TranscriptView(const Conversation& conversation);
  // The nodes before sequence `before` (0: the latest) within the view's
  // bounds, ending on whole turns unless one turn is larger than a page.
  json Page(uint64_t before = 0) const;
  // A node's text in full, by its id, a page from `offset`.
  json Detail(const std::string& id, size_t offset = 0) const;
  // A call's exact retained arguments and result, by its node id.
  json Exchange(const std::string& id, size_t offset = 0) const;
  // The nodes a reader sees whose text contains `query`, ignoring ASCII case,
  // oldest first, at most `limit`: each its id, sequence, kind, how often it
  // contains the query and a snippet around the first time.
  json Search(const std::string& query, size_t limit) const;

 private:
  const Conversation& conversation_;
  const std::map<uint64_t, TranscriptEntry>& Index() const;
  mutable std::once_flag indexed_;
  mutable std::map<uint64_t, TranscriptEntry> entries_;
  // Each finished turn's summary, by turn id.
  mutable std::map<std::string, const json*> summaries_;
};

// Presentation projection only: no system prompts, opaque provider replay,
// base64 images, or arbitrary-path links. Full retained details are paged.
json ConversationView(const Conversation& conversation, uint64_t before = 0);
// A user message's text without the "Attached:" path trailer the model sees.
std::string StripAttachedTrailer(const std::string& text);
// Upserts a checkpoint's nodes and bounds the view once. Returns how many of
// the oldest nodes went; `patches`, when given, receives the {kind:"node"}
// frames of the nodes that are new or different, in view order.
size_t MergeNodes(json& view, const json& nodes, json* patches = nullptr);
// The one reducer of a session's view: every holder of a view (worker, host,
// terminal) folds events through it. False means the event is live-only.
// `patches`, when given, receives the {kind:"node"} frames that bring
// another copy of the view to the same state: whole nodes, or an append to a
// streamed body.
bool ApplySessionEvent(json& state, const std::string& type, const json& data,
                       json* patches = nullptr);
// One page of a long text from `offset`, cut at UTF-8 boundaries, with where
// the next starts and whether there is more.
json PageText(const std::string& text, size_t offset);
json FindHttpExchange(const json& value, const std::string& id);
// The HTTP exchanges of the latest model request, from the display facts.
json LatestHttp(const json& facts);
}  // namespace uagent
#endif  // UAGENT_INCLUDE_AGENT_TRANSCRIPT_H_
