// Copyright 2026 Timon Gentzsch

#include "include/agent/conversation.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/protocol.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/media.h"

namespace uagent {

const char* MessageKindName(MessageKind kind) {
  switch (kind) {
    case MessageKind::kSystem:
      return "system";
    case MessageKind::kProjectInstructions:
      return "project_instructions";
    case MessageKind::kUser:
      return "user";
    case MessageKind::kAssistant:
      return "assistant";
    case MessageKind::kToolResult:
      return "tool_result";
    case MessageKind::kAttachment:
      return "attachment";
    case MessageKind::kEnvironment:
      return "environment";
    case MessageKind::kInternal:
      return "internal";
  }
  return "internal";
}

bool ParseMessageKind(const std::string& name, MessageKind& kind) {
  static constexpr struct {
    const char* name;
    MessageKind kind;
  } kKinds[] = {
      {"system", MessageKind::kSystem},
      {"project_instructions", MessageKind::kProjectInstructions},
      {"user", MessageKind::kUser},
      {"assistant", MessageKind::kAssistant},
      {"tool_result", MessageKind::kToolResult},
      {"attachment", MessageKind::kAttachment},
      {"environment", MessageKind::kEnvironment},
      {"internal", MessageKind::kInternal},
  };
  for (const auto& item : kKinds) {
    if (name == item.name) {
      kind = item.kind;
      return true;
    }
  }
  return false;
}

void Conversation::Reset(json baseline, std::vector<MessageKind> kinds) {
  ResetHistory(std::move(baseline), std::move(kinds));
  archive_ = json::array();
  archive_bytes_ = 0;
  dropped_segments_ = 0;
}

bool Conversation::Restore(json messages, std::vector<MessageKind> kinds,
                           json archive, int64_t dropped_segments) {
  if (!messages.is_array() || messages.empty() ||
      messages.size() != kinds.size() || !archive.is_array()) {
    return false;
  }
  messages_ = std::move(messages);
  kinds_ = std::move(kinds);
  archive_ = std::move(archive);
  archive_bytes_ = archive_.empty()
                       ? 0
                       : static_cast<int64_t>(JsonDump(archive_).size()) - 2;
  dropped_segments_ = std::max(int64_t{0}, dropped_segments);
  return true;
}

void Conversation::ResetHistory(json baseline, std::vector<MessageKind> kinds) {
  messages_ = std::move(baseline);
  kinds_ = std::move(kinds);
}

void Conversation::RefreshBaseline(json system,
                                   const json* project_instructions) {
  if (messages_.empty()) {
    messages_ = json::array({std::move(system)});
    kinds_ = {MessageKind::kSystem};
  } else {
    Set(0, std::move(system), MessageKind::kSystem);
  }
  if (messages_.size() > 1 && kinds_[1] == MessageKind::kProjectInstructions) {
    Erase(1, 2);
  }
  if (project_instructions) {
    messages_.insert(messages_.begin() + 1, *project_instructions);
    kinds_.insert(kinds_.begin() + 1, MessageKind::kProjectInstructions);
  }
}

void Conversation::Push(json message, MessageKind kind) {
  messages_.push_back(std::move(message));
  kinds_.push_back(kind);
}

void Conversation::PopBack() {
  if (messages_.empty()) return;
  messages_.erase(messages_.end() - 1);
  kinds_.pop_back();
}

void Conversation::Set(size_t index, json message, MessageKind kind) {
  messages_[index] = std::move(message);
  kinds_[index] = kind;
}

void Conversation::Erase(size_t begin, size_t end) {
  begin = std::min(begin, messages_.size());
  end = std::min(end, messages_.size());
  if (begin >= end) return;
  messages_.erase(messages_.begin() + static_cast<json::difference_type>(begin),
                  messages_.begin() + static_cast<json::difference_type>(end));
  kinds_.erase(kinds_.begin() + static_cast<std::ptrdiff_t>(begin),
               kinds_.begin() + static_cast<std::ptrdiff_t>(end));
}

bool Conversation::HasKind(MessageKind kind) const {
  return std::find(kinds_.begin(), kinds_.end(), kind) != kinds_.end();
}

std::string Conversation::LastAssistantText() const {
  for (size_t index = messages_.size(); index > 0; --index) {
    const json& message = messages_[index - 1];
    if (kinds_[index - 1] != MessageKind::kAssistant ||
        JsonValue(message, "role", "") != "assistant") {
      continue;
    }
    std::string text = JsonValue(message, "content", "");
    if (!text.empty()) return text;
  }
  return "";
}

std::string Conversation::LastText(MessageKind kind) const {
  for (size_t index = messages_.size(); index > 0; --index) {
    if (kinds_[index - 1] != kind) continue;
    const json& message = messages_[index - 1];
    if (!message.contains("content") || !message["content"].is_string()) {
      continue;
    }
    return message["content"].get<std::string>();
  }
  return "";
}

std::string Conversation::FirstUserText() const {
  for (size_t index = 0; index < messages_.size(); ++index) {
    if (kinds_[index] != MessageKind::kUser) continue;
    const json& message = messages_[index];
    if (JsonValue(message, "role", "") == "user" &&
        message.contains("content") && message["content"].is_string()) {
      return FirstLine(message["content"].get<std::string>());
    }
  }
  return "(no messages)";
}

int64_t Conversation::UserTurns() const {
  return static_cast<int64_t>(
      std::count(kinds_.begin(), kinds_.end(), MessageKind::kUser));
}

size_t Conversation::UserVisibleCount() const {
  return static_cast<size_t>(
      std::count_if(kinds_.begin(), kinds_.end(), [](MessageKind kind) {
        return kind != MessageKind::kProjectInstructions;
      }));
}

std::vector<std::string> Conversation::RecentToolResults(int64_t count) const {
  std::vector<std::string> results;
  for (size_t index = messages_.size();
       index > 0 && static_cast<int64_t>(results.size()) < count; --index) {
    if (kinds_[index - 1] != MessageKind::kToolResult) continue;
    const json& message = messages_[index - 1];
    if (message.contains("content") && message["content"].is_string()) {
      results.push_back(CapResult(message["content"].get<std::string>()));
    }
  }
  std::reverse(results.begin(), results.end());
  return results;
}

size_t Conversation::PruneAttachments(size_t begin) {
  size_t attachments = 0;
  for (size_t index = begin; index < messages_.size(); ++index) {
    json& message = messages_[index];
    if (!message.contains("content")) continue;
    json& content = message["content"];
    if (!content.is_array()) continue;
    attachments += content.empty() ? 0 : content.size() - 1;
    std::string text;
    if (!content.empty() && content[0].is_object()) {
      text = JsonValue(content[0], "text", "");
    }
    content = text + "\n[attachments omitted after processing]";
    if (kinds_[index] == MessageKind::kAttachment) {
      kinds_[index] =
          index == begin ? MessageKind::kUser : MessageKind::kInternal;
    }
  }
  return attachments;
}

size_t Conversation::PruneTurn(size_t turn_start, int64_t turn,
                               int64_t archive_cap, json metadata) {
  if (messages_.size() <= turn_start + 1) return 0;
  if (messages_.size() <= turn_start + 2 && metadata.empty()) return 0;
  size_t before = messages_.size();
  ArchiveRange("trace_pruned", turn_start + 1, messages_.size() - 1, turn,
               archive_cap, std::move(metadata));
  json answer = messages_.back();
  MessageKind answer_kind = kinds_.back();
  Erase(turn_start + 1, messages_.size());
  Push(std::move(answer), answer_kind);
  return before - messages_.size();
}

size_t Conversation::StripImageParts() {
  return StripImageContentParts(messages_);
}

void Conversation::ArchiveRange(const char* reason, size_t begin, size_t end,
                                int64_t turn, int64_t archive_cap,
                                json metadata) {
  if (!metadata.is_object()) metadata = json::object();
  end = std::min(end, messages_.size());
  json saved = json::array();
  json saved_kinds = json::array();
  if (begin < end) {
    for (size_t index = begin; index < end; ++index) {
      saved.push_back(messages_[index]);
      saved_kinds.push_back(MessageKindName(kinds_[index]));
    }
  }
  if (saved.empty() && metadata.empty()) return;
  json segment = {{"turn", turn},
                  {"reason", reason},
                  {"messages", std::move(saved)},
                  {"message_kinds", std::move(saved_kinds)}};
  for (auto& [key, value] : metadata.items()) {
    if (!segment.contains(key)) segment[key] = std::move(value);
  }
  AddArchiveSegment(std::move(segment), archive_cap);
}

void Conversation::ArchiveAll(const char* reason, size_t baseline_size,
                              int64_t turn, int64_t archive_cap) {
  ArchiveRange(reason, baseline_size, messages_.size(), turn, archive_cap);
}

void Conversation::AddArchiveSegment(json segment, int64_t archive_cap) {
  int64_t segment_bytes = static_cast<int64_t>(JsonDump(segment).size());
  if (archive_cap <= 0 || segment_bytes > archive_cap) {
    ++dropped_segments_;
    return;
  }
  int64_t bytes = segment_bytes + (archive_.empty() ? 0 : 1);
  while (!archive_.empty() && archive_bytes_ + bytes > archive_cap) {
    archive_bytes_ -= static_cast<int64_t>(JsonDump(archive_.front()).size()) +
                      (archive_.size() > 1 ? 1 : 0);
    archive_.erase(archive_.begin());
    ++dropped_segments_;
    bytes = segment_bytes + (archive_.empty() ? 0 : 1);
  }
  archive_.push_back(std::move(segment));
  archive_bytes_ += segment_bytes + (archive_.size() > 1 ? 1 : 0);
}

json MessageKindsJson(const std::vector<MessageKind>& kinds) {
  json value = json::array();
  for (MessageKind kind : kinds) value.push_back(MessageKindName(kind));
  return value;
}

bool ParseMessageKinds(const json& value, size_t expected,
                       std::vector<MessageKind>& kinds) {
  if (!value.is_array() || value.size() != expected) return false;
  std::vector<MessageKind> parsed;
  parsed.reserve(value.size());
  for (const json& item : value) {
    if (!item.is_string()) return false;
    MessageKind kind;
    if (!ParseMessageKind(item.get<std::string>(), kind)) return false;
    parsed.push_back(kind);
  }
  kinds = std::move(parsed);
  return true;
}

}  // namespace uagent
