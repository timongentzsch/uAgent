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
  kUser,
  kAssistant,
  kToolResult,
  kAttachment,
  kInternal,
};

const char* MessageKindName(MessageKind kind);
bool ParseMessageKind(const std::string& name, MessageKind& kind);

class Conversation {
 public:
  const json& Messages() const { return messages_; }
  const json& Archive() const { return archive_; }
  const std::vector<MessageKind>& Kinds() const { return kinds_; }

  bool Empty() const { return messages_.empty(); }
  size_t Size() const { return messages_.size(); }
  const json& At(size_t index) const { return messages_[index]; }
  MessageKind KindAt(size_t index) const { return kinds_[index]; }

  size_t ArchivedSegments() const { return archive_.size(); }
  int64_t ArchivedBytes() const { return archive_bytes_; }
  int64_t DroppedSegments() const { return dropped_segments_; }

  void Reset(json baseline, std::vector<MessageKind> kinds);
  bool Restore(json messages, std::vector<MessageKind> kinds, json archive,
               int64_t dropped_segments);
  void ResetHistory(json baseline, std::vector<MessageKind> kinds);
  void RefreshBaseline(json system, const json* project_instructions);

  void Push(json message, MessageKind kind);
  void PopBack();
  void Set(size_t index, json message, MessageKind kind);
  void Erase(size_t begin, size_t end);

  std::string LastAssistantText() const;
  std::string FirstUserText() const;
  int64_t UserTurns() const;
  size_t UserVisibleCount() const;
  std::vector<std::string> RecentToolResults(int64_t count) const;

  size_t PruneAttachments(size_t begin);
  size_t PruneTurn(size_t turn_start, int64_t turn, int64_t archive_cap,
                   json metadata);
  size_t StripImageParts();

  void ArchiveRange(const char* reason, size_t begin, size_t end, int64_t turn,
                    int64_t archive_cap, json metadata = json::object());
  void ArchiveAll(const char* reason, size_t baseline_size, int64_t turn,
                  int64_t archive_cap);

 private:
  void AddArchiveSegment(json segment, int64_t archive_cap);

  json messages_ = json::array();
  std::vector<MessageKind> kinds_;
  json archive_ = json::array();
  int64_t archive_bytes_ = 0;
  int64_t dropped_segments_ = 0;
};

json MessageKindsJson(const std::vector<MessageKind>& kinds);
bool ParseMessageKinds(const json& value, size_t expected,
                       std::vector<MessageKind>& kinds);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_CONVERSATION_H_
