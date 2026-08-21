// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_CONVERSATION_H_
#define UAGENT_INCLUDE_AGENT_CONVERSATION_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "include/core/json.h"

namespace uagent {

enum class MessageKind {
  kSystem,
  kProjectInstructions,
  kMemory,
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

class Conversation {
 public:
  json& Messages() { return messages_; }
  const json& Messages() const { return messages_; }
  const json& Archive() const { return archive_; }
  const std::vector<MessageKind>& Kinds() const { return kinds_; }
  // Rendered tool receipts, keyed by call id. The model never sees these; they
  // exist so a resumed transcript can redraw a diff instead of a grey line.
  const json& ToolDisplays() const { return tool_displays_; }

  bool Empty() const { return messages_.empty(); }
  size_t Size() const { return messages_.size(); }
  const json& At(size_t index) const { return messages_[index]; }
  MessageKind KindAt(size_t index) const { return kinds_[index]; }
  bool HasKind(MessageKind kind) const;

  size_t ArchivedSegments() const { return archive_.size(); }
  int64_t ArchivedBytes() const { return archive_bytes_; }
  int64_t DroppedSegments() const { return dropped_segments_; }

  void Reset(json baseline, std::vector<MessageKind> kinds);
  bool Restore(json messages, std::vector<MessageKind> kinds, json archive,
               int64_t dropped_segments, json tool_displays = json::object());
  void ResetHistory(json baseline, std::vector<MessageKind> kinds);
  void RefreshBaseline(json system);

  // Keeps the receipt only while its call is still in the transcript, so a
  // compacted turn takes its diffs with it.
  void RecordToolDisplay(const std::string& call_id, std::string display);
  const std::string* ToolDisplay(const std::string& call_id) const;

  void Push(json message, MessageKind kind);
  void Upsert(json message, MessageKind kind);
  void UpsertTail(json message, MessageKind kind);
  void Set(size_t index, json message, MessageKind kind);
  void Erase(size_t begin, size_t end);

  std::string LastAssistantText() const;
  std::string LastText(MessageKind kind) const;
  std::string FirstUserText() const;
  int64_t UserTurns() const;
  size_t UserVisibleCount() const;
  bool HasRecentToolResult(const std::string& name,
                           const std::string& arguments,
                           const std::string& result) const;
  ToolTracePruneResult PruneOldToolResults(
      size_t protect_chars, size_t minimum_reclaim_chars,
      const std::vector<std::string>& retained_tools);

  size_t PruneAttachments(size_t begin);
  void ArchiveTurn(size_t turn_start, int64_t turn, int64_t archive_cap,
                   json metadata);

  void ArchiveRange(const char* reason, size_t begin, size_t end, int64_t turn,
                    int64_t archive_cap, json metadata = json::object());
  void ArchiveAll(const char* reason, size_t baseline_size, int64_t turn,
                  int64_t archive_cap);

 private:
  void AddArchiveSegment(json segment, int64_t archive_cap);

  void PruneToolDisplays();

  json messages_ = json::array();
  std::vector<MessageKind> kinds_;
  json tool_displays_ = json::object();
  json archive_ = json::array();
  int64_t archive_bytes_ = 0;
  int64_t dropped_segments_ = 0;
};

json MessageKindsJson(const std::vector<MessageKind>& kinds);
bool ParseMessageKinds(const json& value, size_t expected,
                       std::vector<MessageKind>& kinds);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_CONVERSATION_H_
