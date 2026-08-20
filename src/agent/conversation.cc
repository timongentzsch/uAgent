// Copyright 2026 Timon Gentzsch

#include "include/agent/conversation.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "include/agent/protocol.h"
#include "include/core/checked.h"
#include "include/core/json.h"
#include "include/core/strings.h"

namespace uagent {

namespace {

void NormalizeRole(json& message, MessageKind kind) {
  switch (kind) {
    case MessageKind::kUser:
    case MessageKind::kAttachment:
      message["role"] = "user";
      return;
    case MessageKind::kAssistant:
      message["role"] = "assistant";
      return;
    case MessageKind::kToolResult:
      // Native results keep their tool_call_id and role. Text-protocol results
      // have no native call to reference, so they are harness-owned context.
      if (JsonValue(message, "role", "") != "tool") {
        message["role"] = "user";
      }
      return;
    case MessageKind::kSystem:
      message["role"] = "system";
      return;
    // Harness-injected context. Only the baseline at index zero may be
    // `system`: the OpenAI convention expects a single leading system message,
    // and strict chat templates reject a later one outright. Project
    // instructions and memory appear here only when resuming a session saved
    // before the baseline was consolidated.
    case MessageKind::kProjectInstructions:
    case MessageKind::kMemory:
    case MessageKind::kRuntimeContext:
    case MessageKind::kInternal:
      message["role"] = "user";
      return;
  }
}

void NormalizeRoles(json& messages, const std::vector<MessageKind>& kinds) {
  for (size_t index = 0; index < messages.size(); ++index) {
    NormalizeRole(messages[index], kinds[index]);
  }
}

// One list, both directions: the persisted names and the enum never drift.
constexpr struct {
  const char* name;
  MessageKind kind;
} kKinds[] = {
    {"system", MessageKind::kSystem},
    {"project_instructions", MessageKind::kProjectInstructions},
    {"memory", MessageKind::kMemory},
    {"user", MessageKind::kUser},
    {"assistant", MessageKind::kAssistant},
    {"tool_result", MessageKind::kToolResult},
    {"attachment", MessageKind::kAttachment},
    {"runtime_context", MessageKind::kRuntimeContext},
    {"internal", MessageKind::kInternal},
};

constexpr size_t kMinimumPrunableResultChars = 1024;
constexpr int64_t kProtectedUserTurns = 2;
constexpr std::string_view kCompactedToolOutput = "[old tool output compacted:";

const json* FindToolCallFunction(const json& message, const std::string& id) {
  if (!message.is_object() || JsonValue(message, "role", "") != "assistant" ||
      !message.contains("tool_calls") || !message["tool_calls"].is_array()) {
    return nullptr;
  }
  for (const json& call : message["tool_calls"]) {
    if (JsonValue(call, "id", "") != id || !call.contains("function") ||
        !call["function"].is_object()) {
      continue;
    }
    return &call["function"];
  }
  return nullptr;
}

std::string ToolCallName(const json& message, const std::string& id) {
  const json* function = FindToolCallFunction(message, id);
  return function ? JsonValue(*function, "name", "") : "";
}

std::string ToolResultName(const json& messages,
                           const std::vector<MessageKind>& kinds,
                           size_t result_index) {
  const json& message = messages[result_index];
  std::string id = JsonValue(message, "tool_call_id", "");
  if (!id.empty()) {
    for (size_t index = result_index; index > 0; --index) {
      std::string name = ToolCallName(messages[index - 1], id);
      if (!name.empty()) return name;
      if (kinds[index - 1] == MessageKind::kUser) break;
    }
  }

  std::string content = JsonValue(message, "content", "");
  constexpr std::string_view kPrefix = "[tool_result ";
  if (!content.starts_with(kPrefix)) return "";
  size_t end = content.find(']', kPrefix.size());
  return end == std::string::npos
             ? ""
             : content.substr(kPrefix.size(), end - kPrefix.size());
}

std::string CompactedResult(const std::string& content) {
  std::string preview = Utf8Trunc(FirstLine(content), 160);
  return std::string(kCompactedToolOutput) + " " +
         std::to_string(content.size()) +
         " chars; full result retained in "
         "session trace" +
         (preview.empty() ? "]" : "; preview: " + preview + "]");
}

bool MatchingToolCall(const json& message, const std::string& id,
                      const std::string& name, const std::string& arguments) {
  const json* function = FindToolCallFunction(message, id);
  return function && JsonValue(*function, "name", "") == name &&
         JsonValue(*function, "arguments", "") == arguments;
}

}  // namespace

const char* MessageKindName(MessageKind kind) {
  for (const auto& item : kKinds) {
    if (item.kind == kind) return item.name;
  }
  return "internal";
}

bool ParseMessageKind(const std::string& name, MessageKind& kind) {
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
  NormalizeRoles(messages, kinds);
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
  NormalizeRoles(baseline, kinds);
  messages_ = std::move(baseline);
  kinds_ = std::move(kinds);
}

void Conversation::RefreshBaseline(json system) {
  if (messages_.empty()) {
    messages_ = json::array({std::move(system)});
    kinds_ = {MessageKind::kSystem};
  } else {
    Set(0, std::move(system), MessageKind::kSystem);
  }
  // Sessions saved before the baseline was consolidated still carry separate
  // project-instruction and memory messages. Their content is already folded
  // into the system message above, so drop the stale copies.
  while (messages_.size() > 1 &&
         (kinds_[1] == MessageKind::kProjectInstructions ||
          kinds_[1] == MessageKind::kMemory)) {
    Erase(1, 2);
  }
}

void Conversation::Push(json message, MessageKind kind) {
  NormalizeRole(message, kind);
  messages_.push_back(std::move(message));
  kinds_.push_back(kind);
}

void Conversation::Upsert(json message, MessageKind kind) {
  for (size_t index = kinds_.size(); index > 0; --index) {
    if (kinds_[index - 1] == kind) {
      Set(index - 1, std::move(message), kind);
      return;
    }
  }
  Push(std::move(message), kind);
}

// Refresh volatile harness context at the tail so a change (terminal width,
// date) never invalidates the cacheable prefix of the history.
void Conversation::UpsertTail(json message, MessageKind kind) {
  NormalizeRole(message, kind);
  if (!kinds_.empty() && kinds_.back() == kind && messages_.back() == message) {
    return;
  }
  for (size_t index = kinds_.size(); index > 0; --index) {
    if (kinds_[index - 1] == kind) {
      Erase(index - 1, index);
    }
  }
  Push(std::move(message), kind);
}

void Conversation::Set(size_t index, json message, MessageKind kind) {
  NormalizeRole(message, kind);
  // Byte-identical replacement changes nothing observable: leave the cached
  // prefix untouched instead of rewriting message zero on every refresh.
  if (messages_[index] == message && kinds_[index] == kind) return;
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

bool Conversation::HasRecentToolResult(const std::string& name,
                                       const std::string& arguments,
                                       const std::string& result) const {
  int64_t user_turns = 0;
  for (size_t index = messages_.size(); index > 0; --index) {
    size_t current = index - 1;
    if (kinds_[current] == MessageKind::kUser) {
      if (++user_turns >= kProtectedUserTurns) break;
      continue;
    }
    if (kinds_[current] != MessageKind::kToolResult) continue;
    const json& message = messages_[current];
    if (JsonValue(message, "content", "") != result) continue;
    std::string id = JsonValue(message, "tool_call_id", "");
    if (id.empty()) continue;
    for (size_t call_index = current; call_index > 0; --call_index) {
      const json& candidate = messages_[call_index - 1];
      if (MatchingToolCall(candidate, id, name, arguments)) return true;
      if (kinds_[call_index - 1] == MessageKind::kUser) break;
    }
  }
  return false;
}

ToolTracePruneResult Conversation::PruneOldToolResults(
    size_t protect_chars, size_t minimum_reclaim_chars,
    const std::vector<std::string>& retained_tools) {
  if (minimum_reclaim_chars == 0) return {};
  const std::unordered_set<std::string> retained(retained_tools.begin(),
                                                 retained_tools.end());
  struct Candidate {
    size_t index;
    std::string replacement;
  };
  std::vector<Candidate> candidates;
  size_t protected_chars = 0;
  size_t reclaimable_chars = 0;
  int64_t user_turns = 0;

  for (size_t index = messages_.size(); index > 0; --index) {
    size_t current = index - 1;
    if (kinds_[current] == MessageKind::kUser) {
      ++user_turns;
      continue;
    }
    if (user_turns < kProtectedUserTurns ||
        kinds_[current] != MessageKind::kToolResult) {
      continue;
    }
    json& message = messages_[current];
    const std::string* content = JsonStringRef(message, "content");
    if (!content || content->size() < kMinimumPrunableResultChars ||
        content->starts_with(kCompactedToolOutput) ||
        retained.contains(ToolResultName(messages_, kinds_, current))) {
      continue;
    }
    protected_chars = SaturatingAdd(protected_chars, content->size());
    if (protected_chars <= protect_chars) continue;
    std::string replacement = CompactedResult(*content);
    if (replacement.size() >= content->size()) continue;
    size_t reclaimed = content->size() - replacement.size();
    reclaimable_chars = SaturatingAdd(reclaimable_chars, reclaimed);
    candidates.push_back({current, std::move(replacement)});
  }

  if (reclaimable_chars < minimum_reclaim_chars) return {};
  for (Candidate& candidate : candidates) {
    messages_[candidate.index]["content"] = std::move(candidate.replacement);
  }
  return {candidates.size(), reclaimable_chars};
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

void Conversation::ArchiveTurn(size_t turn_start, int64_t turn,
                               int64_t archive_cap, json metadata) {
  if (messages_.size() <= turn_start + 1) return;
  if (messages_.size() <= turn_start + 2 && metadata.empty()) return;
  ArchiveRange("tool_trace", turn_start + 1, messages_.size() - 1, turn,
               archive_cap, std::move(metadata));
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
