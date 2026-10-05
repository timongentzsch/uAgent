// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_CONVERSATION_H_
#define UAGENT_INCLUDE_AGENT_CONVERSATION_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "include/core/json.h"

namespace uagent {

enum class MessageKind {
  kSystem,
  kUser,
  kAssistant,
  kToolResult,
  kAttachment,
  kRuntimeContext,
  kInternal,
};

const char* MessageKindName(MessageKind kind);
bool ParseMessageKind(const std::string& name, MessageKind& kind);

struct ToolTracePruneResult {
  size_t results = 0;
  size_t reclaimed_chars = 0;
};

// A message the person wrote (a prompt or guidance, with or without files),
// not files attached on request mid-turn, which share its role and kind.
bool IsUserMessage(const json& message, MessageKind kind);

class Conversation {
 public:
  const json& Messages() const { return messages_; }
  int64_t ArchivedBytes() const { return archive_bytes_; }
  const json& Archive() const { return archive_; }
  // The archive as JSON text, joined from each segment's serialization kept
  // since it was archived: a save never re-dumps the archive.
  std::string ArchiveText() const;
  const std::vector<MessageKind>& Kinds() const { return kinds_; }
  json DisplayMetadata() const;
  const json& Statistics() const { return statistics_; }
  void AddStatistics(const json& delta);
  const std::vector<uint64_t>& DisplayIds() const { return display_ids_; }
  const json& DisplayFacts() const { return display_facts_; }
  void RecordDisplay(const std::string& key, json facts);
  // Delivery receipts already announced as terminal notices. Display facts
  // are evictable, so they cannot dedupe the per-request receipt: this small
  // map (display id -> last announced deliveries array) survives fact
  // eviction and session restore, and only the request path writes it.
  json AnnouncedDeliveries(const std::string& id) const;
  void RecordAnnouncedDeliveries(std::string id, json values);
  // When a user message arrived (UTC), by display id; empty when unknown.
  // Kept apart from the evictable display facts: a coordinator's model reads
  // these stamps, so losing one would change its cached prefix.
  std::string Arrival(uint64_t id) const;
  json RecordEntry(json facts);
  std::string LastDisplayId() const;

  bool Empty() const { return messages_.empty(); }
  size_t Size() const { return messages_.size(); }
  const json& At(size_t index) const { return messages_[index]; }
  MessageKind KindAt(size_t index) const { return kinds_[index]; }
  bool HasKind(MessageKind kind) const;

  int64_t DroppedSegments() const { return dropped_segments_; }

  void Reset(json baseline, std::vector<MessageKind> kinds);
  bool Restore(json messages, std::vector<MessageKind> kinds, json archive,
               int64_t dropped_segments, const json& display = json::object());
  void ResetHistory(json baseline, std::vector<MessageKind> kinds);
  void RefreshBaseline(json system);

  void Push(json message, MessageKind kind);
  // Same as Push, but the message keeps a pre-existing display id (the
  // post-compaction re-push). The id counter advances past it so fresh
  // mints stay unique.
  void PushWithDisplayId(json message, MessageKind kind, uint64_t id);
  void Set(size_t index, json message, MessageKind kind);
  void Erase(size_t begin, size_t end);
  // Drops the Nth user message and everything after it (message-exclusive,
  // so it can be edited and sent again). False when out of range, leaving
  // the conversation untouched.
  bool TruncateBeforeUserTurn(int64_t turn);
  // The 1-based number of the user message shown as `m-<display id>`, 0 when
  // it is not a user message or no longer live (compacted away).
  int64_t UserMessageNumber(uint64_t display_id) const;
  // The text of the Nth user message, as it was sent.
  std::string UserMessageText(int64_t turn) const;

  std::string LastAssistantText() const;
  std::string LastText(MessageKind kind) const;
  std::string FirstUserText() const;
  int64_t UserTurns() const;
  bool HasRecentToolResult(const std::string& name,
                           const std::string& arguments,
                           const std::string& result) const;
  // Replaces old tool results by a short note of what they were, keeping
  // the newest `protect_chars` of them. Results of the last two user turns
  // stay whole unless `within_turn`, which is for a turn that has itself
  // grown long.
  ToolTracePruneResult PruneOldToolResults(
      size_t protect_chars, size_t minimum_reclaim_chars,
      const std::vector<std::string>& retained_tools, bool within_turn = false);

  size_t PruneAttachments(size_t begin, const std::string& route = "");
  void ArchiveTurn(size_t turn_start, int64_t turn, int64_t archive_cap,
                   json metadata);

  bool ArchiveRange(const char* reason, size_t begin, size_t end, int64_t turn,
                    int64_t archive_cap, json metadata = json::object());
  void ArchiveAll(const char* reason, size_t baseline_size, int64_t turn,
                  int64_t archive_cap);

 private:
  bool AddArchiveSegment(json segment, int64_t archive_cap);

  // Parallel by index and the same length, always. Every mutator below writes
  // both, Restore rejects a mismatched pair off disk, and the read paths index
  // kinds_ with a bound taken from messages_ -- so a new mutator that touches
  // one array has to touch the other. The pair is kept rather than merged into
  // one vector of {message, kind} because Messages() is handed straight to the
  // request path as a reference; a merged store would have to materialize a
  // json array per model request.
  json messages_ = json::array();
  std::vector<MessageKind> kinds_;
  std::vector<uint64_t> display_ids_;
  json display_facts_ = json::object();
  // Serialized sizes of display_facts_ entries, kept in lockstep so fact
  // eviction can drop the largest entry without re-serializing the store.
  std::map<std::string, size_t> fact_bytes_;
  // Last announced attachment deliveries per display id. Tiny (a few rows
  // of name/delivery/path), bounded below, never evicted for space.
  json announced_deliveries_ = json::object();
  // User message arrivals (display id -> UTC time), the newest kArrivals.
  static constexpr size_t kArrivals = 4096;
  std::map<uint64_t, std::string> arrivals_;
  json statistics_ = {{"complete", true}};
  uint64_t next_display_id_ = 1;
  size_t display_bytes_ = 0;
  json archive_ = json::array();
  std::vector<std::string> archive_texts_;  // archive_, one text per segment
  int64_t archive_bytes_ = 0;
  int64_t dropped_segments_ = 0;
};

json MessageKindsJson(const std::vector<MessageKind>& kinds);
bool ParseMessageKinds(const json& value, size_t expected,
                       std::vector<MessageKind>& kinds);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_CONVERSATION_H_
