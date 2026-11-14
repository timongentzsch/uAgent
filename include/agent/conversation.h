// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_CONVERSATION_H_
#define UAGENT_INCLUDE_AGENT_CONVERSATION_H_
// The conversation: its messages by kind, the archive of compacted ones and
// the size estimates compaction works from.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
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

// A message the person wrote, not files attached mid-turn in the same role.
bool IsUserMessage(const json& message, MessageKind kind);

class Conversation {
 public:
  const json& Messages() const { return messages_; }
  const json& Archive() const { return archive_; }
  // Bytes the parsed conversation holds, for budgeting caches of it.
  size_t EstimatedBytes() const;
  // Captured on the conversation thread; readers keep an immutable revision.
  std::shared_ptr<const json> SharedArchive();
  // Joined from each segment's kept serialization: a save never re-dumps it.
  std::string ArchiveText() const;
  const std::vector<MessageKind>& Kinds() const { return kinds_; }
  json DisplayMetadata() const;
  const json& Statistics() const { return statistics_; }
  void AddStatistics(const json& delta);
  const std::vector<uint64_t>& DisplayIds() const { return display_ids_; }
  const json& DisplayFacts() const { return display_facts_; }
  void RecordDisplay(const std::string& key, json facts);
  // Deliveries already announced, by display id; outlives fact eviction.
  json AnnouncedDeliveries(const std::string& id) const;
  void RecordAnnouncedDeliveries(std::string id, json values);
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
  // Push keeping an existing display id; the counter advances past it.
  void PushWithDisplayId(json message, MessageKind kind, uint64_t id);
  void Set(size_t index, json message, MessageKind kind);
  void Erase(size_t begin, size_t end);
  // Drops the Nth user message and all after it; false when out of range.
  bool TruncateBeforeUserTurn(int64_t turn);
  // 1-based number of user message `m-<display id>`; 0 when it is not live.
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
  ToolTracePruneResult PruneOldToolResults(
      size_t protect_chars, size_t minimum_reclaim_chars,
      const std::vector<std::string>& retained_tools);

  size_t PruneAttachments(size_t begin, const std::string& route = "");
  void ArchiveTurn(size_t turn_start, int64_t turn, int64_t archive_cap);

  bool ArchiveRange(const char* reason, size_t begin, size_t end, int64_t turn,
                    int64_t archive_cap);
  void ArchiveAll(const char* reason, size_t baseline_size, int64_t turn,
                  int64_t archive_cap);

 private:
  bool AddArchiveSegment(json segment, int64_t archive_cap);

  // Parallel by index and always the same length. Not merged into one vector:
  // Messages() is handed to the request path by reference.
  json messages_ = json::array();
  std::vector<MessageKind> kinds_;
  std::vector<uint64_t> display_ids_;
  json display_facts_ = json::object();
  // Serialized size of each display_facts_ entry, for evicting the largest.
  std::map<std::string, size_t> fact_bytes_;
  // Last announced deliveries per display id; bounded, never evicted for space.
  json announced_deliveries_ = json::object();
  json statistics_ = {{"complete", true}};
  uint64_t next_display_id_ = 1;
  size_t display_bytes_ = 0;
  json archive_ = json::array();
  std::shared_ptr<const json> shared_archive_;
  std::vector<std::string> archive_texts_;  // archive_, one text per segment
  int64_t archive_bytes_ = 0;
  int64_t dropped_segments_ = 0;
};

// One item per line as the model reads it: "- [pending] Fix the lexer".
std::string TodoText(const json& items);
// {done, total, current (the first item not done)}; null for an empty list.
json TodoProgress(const json& items);

json MessageKindsJson(const std::vector<MessageKind>& kinds);
bool ParseMessageKinds(const json& value, size_t expected,
                       std::vector<MessageKind>& kinds);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_CONVERSATION_H_
