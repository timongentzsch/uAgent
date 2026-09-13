// Copyright 2026 Timon Gentzsch

#include "include/agent/session_view.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "include/core/fs.h"
#include "include/core/strings.h"

namespace uagent {
namespace {
constexpr size_t kViewBytes = size_t{384} * 1024;
constexpr size_t kViewBlocks = 64;
struct Entry {
  const json* message;
  std::string kind;
};
bool Visible(const Entry& entry) {
  return entry.kind == "user" || entry.kind == "assistant" ||
         entry.kind == "tool_result" || entry.kind == "attachment" ||
         entry.kind == "activity" || entry.kind == "turn_summary" ||
         entry.kind == "compaction";
}
std::map<uint64_t, Entry> Entries(const Conversation& conversation) {
  std::map<uint64_t, Entry> entries;
  for (const json& segment : conversation.Archive()) {
    const json* messages = JsonArray(segment, "messages");
    const json* kinds = JsonArray(segment, "message_kinds");
    const json* ids = JsonArray(segment, "display_ids");
    if (!messages || !kinds || !ids || kinds->size() != messages->size() ||
        ids->size() != messages->size()) {
      continue;
    }
    for (size_t i = 0; i < messages->size(); ++i) {
      if (!(*ids)[i].is_number_unsigned()) continue;
      const auto id = (*ids)[i].get<uint64_t>();
      if (!id) continue;
      entries.try_emplace(
          id, Entry{&(*messages)[i], (*kinds)[i].is_string()
                                         ? (*kinds)[i].get<std::string>()
                                         : "internal"});
    }
  }
  const auto& ids = conversation.DisplayIds();
  for (size_t i = 0; i < conversation.Size(); ++i) {
    const uint64_t id = ids[i];
    entries.try_emplace(id, Entry{&conversation.At(i),
                                  MessageKindName(conversation.KindAt(i))});
  }
  for (const auto& facts : conversation.DisplayFacts()) {
    std::string kind = JsonValue(facts, "kind", "");
    if (kind == "activity" || kind == "turn_summary" || kind == "compaction") {
      entries.try_emplace(JsonValue(facts, "sequence", uint64_t{0}),
                          Entry{&facts, kind});
    }
  }
  return entries;
}

std::string Text(const json& message) {
  if (JsonValue(message, "kind", "") == "activity") {
    return JsonValue(message, "text", "");
  }
  if (const json* parts = JsonArray(message, "content")) {
    std::string text;
    for (const json& part : *parts) {
      if (JsonValue(part, "type", "") == "text") {
        text += JsonValue(part, "text", "");
      }
    }
    return text;
  }
  return JsonValue(message, "content", "");
}
}  // namespace

void MergeDisplayBlock(json& view, const json& block) {
  json& blocks = view["blocks"];
  if (!blocks.is_array()) blocks = json::array();
  auto found =
      std::find_if(blocks.begin(), blocks.end(), [&](const json& item) {
        return JsonValue(item, "id", "") == JsonValue(block, "id", "");
      });
  if (found == blocks.end()) {
    blocks.push_back(block);
  } else {
    *found = block;
  }
  size_t bytes = JsonEstimatedBytes(blocks);
  while (!blocks.empty() &&
         (blocks.size() > kViewBlocks || bytes > kViewBytes)) {
    bytes -= std::min(bytes, JsonEstimatedBytes(blocks.front()));
    blocks.erase(blocks.begin());
    view["more"] = true;
  }
  if (!blocks.empty()) {
    view["before"] = JsonValue(blocks.front(), "sequence", uint64_t{0});
  }
}

bool ApplySessionEvent(json& state, const std::string& type, const json& data) {
  if (type == "usage.updated") {
    state["usage"] = data["usage"];
    if (data.contains("context_tokens")) {
      state["context_tokens"] = data["context_tokens"];
    }
    if (data.contains("statistics")) state["statistics"] = data["statistics"];
  } else if (type == "message.changed") {
    MergeDisplayBlock(state["view"], data["block"]);
  } else if (type == "activities.changed") {
    state["activities"] = data["activities"];
  } else if (type == "collaborator.changed") {
    json& rows = state["collaborators"];
    if (!rows.is_array()) rows = json::array();
    const json& changed = data["collaborator"];
    const std::string id = JsonValue(changed, "id", "");
    auto found = std::find_if(rows.begin(), rows.end(), [&](const json& row) {
      return JsonValue(row, "id", "") == id;
    });
    if (JsonValue(data, "removed", false)) {
      if (found != rows.end()) rows.erase(found);
    } else if (!id.empty()) {
      if (found == rows.end()) {
        rows.push_back(changed);
      } else {
        *found = changed;
      }
    }
  } else if (type == "http.exchange") {
    state["http"] = json::array({data});
  } else if (type == "config.changed" && data.contains("permissions")) {
    state["permissions"] = data["permissions"];
    state["yolo"] = JsonValue(data["permissions"], "effective", "") == "yolo";
  } else {
    return false;
  }
  return true;
}

namespace {
json DisplayBlock(const Conversation& conversation, uint64_t sequence,
                  const Entry& entry) {
  const json& facts = conversation.DisplayFacts();
  std::string id = "m-" + std::to_string(sequence);
  const json& message = *entry.message;
  std::string text = Text(message);
  json block = {{"id", id},
                {"sequence", sequence},
                {"kind", entry.kind},
                {"text", Utf8Trunc(text, 4096)},
                {"truncated", text.size() > 4096}};
  json metadata = JsonValue(facts, id.c_str(), json::object());
  for (const char* key :
       {"time",      "incoming",        "activity_id",    "agent_id",
        "status",    "request_id",      "route",          "duration_ms",
        "ttft_ms",   "usage",           "usage_reported", "tokens_per_second",
        "turn_root", "reply_to",        "reply_excerpt",  "http",
        "files",     "summary",         "deliveries",     "activity",
        "origin",    "source_call_ids", "compaction",     "memory"}) {
    if (metadata.contains(key)) block[key] = metadata[key];
  }
  if (entry.kind == "assistant") {
    std::string reasoning = JsonValue(metadata, "reasoning", "");
    if (reasoning.empty()) {
      reasoning = JsonValue(message, "reasoning_content",
                            JsonValue(message, "reasoning", ""));
    }
    block["reasoning"] = Utf8Trunc(reasoning, 4096);
    block["reasoning_available"] = !reasoning.empty();
    if (const json* calls = JsonArray(message, "tool_calls")) {
      block["tools"] = json::array();
      for (const json& call : *calls) {
        std::string call_id = JsonValue(call, "id", "");
        json function = JsonValue(call, "function", json::object());
        json tool = {
            {"id", call_id},
            {"name", Utf8Trunc(JsonValue(function, "name", ""), 128)},
            {"arguments",
             Utf8Trunc(JsonValue(function, "arguments", ""), 1024)},
            {"status", JsonValue(JsonValue(facts, ("t-" + call_id).c_str(),
                                           json::object()),
                                 "status", "not recorded")}};
        tool["activity"] = JsonValue(
            JsonValue(facts, ("t-" + call_id).c_str(), json::object()),
            "activity", json::object());
        block["tools"].push_back(std::move(tool));
        if (block["tools"].size() >= 32) {
          break;
        }
      }
    }
  }
  if (entry.kind == "tool_result") {
    std::string call_id = JsonValue(message, "tool_call_id", "");
    json detail = JsonValue(facts, ("t-" + call_id).c_str(), json::object());
    block["call_id"] = call_id;
    block["activity"] = JsonValue(detail, "activity", json::object());
    block["name"] = JsonValue(detail, "name", "tool");
    block["status"] = JsonValue(detail, "status", "not recorded");
    if (detail.contains("duration_ms")) {
      block["duration_ms"] = detail["duration_ms"];
    }
    block["detail_id"] = "t-" + call_id;
    block["change"] = Utf8Trunc(JsonValue(detail, "change", ""), 4096);
    block["artifact"] = detail.contains("artifact");
    if (detail.contains("exchange_path")) {
      block["exchange_path"] = detail["exchange_path"];
    }
  }
  if (const json* parts = JsonArray(message, "content")) {
    block["unavailable_images"] =
        std::count_if(parts->begin(), parts->end(), [](const json& part) {
          return JsonValue(part, "type", "") == "image_url";
        });
  }
  return block;
}
}  // namespace

json LastMessageView(const Conversation& conversation) {
  if (conversation.Empty()) return nullptr;
  const size_t last = conversation.Size() - 1;
  Entry entry{&conversation.At(last),
              MessageKindName(conversation.KindAt(last))};
  return Visible(entry) ? DisplayBlock(conversation,
                                       conversation.DisplayIds()[last], entry)
                        : json(nullptr);
}

json ConversationView(const Conversation& conversation, uint64_t before) {
  auto entries = Entries(conversation);
  std::vector<json> blocks;
  size_t bytes = 0;
  uint64_t first = 0;
  bool more = false;
  for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
    if (before && it->first >= before) {
      continue;
    }
    const Entry& entry = it->second;
    if (!Visible(entry)) {
      continue;
    }
    json block = DisplayBlock(conversation, it->first, entry);
    bytes += JsonDump(block).size();
    if (bytes > kViewBytes || blocks.size() >= kViewBlocks) {
      more = true;
      break;
    }
    first = it->first;
    blocks.push_back(std::move(block));
  }
  std::reverse(blocks.begin(), blocks.end());
  return {
      {"fork",
       JsonValue(conversation.DisplayFacts(), "fork-origin", json(nullptr))},
      {"blocks", blocks},
      {"before", first},
      {"more", more},
      {"dropped_segments", conversation.DroppedSegments()},
      {"retention", "Full retained content; missing facts are not inferred."}};
}

json ConversationDetail(const Conversation& conversation, const std::string& id,
                        size_t offset) {
  constexpr size_t kPage = size_t{16} * 1024;
  std::string text;
  const json& facts = conversation.DisplayFacts();
  if (id.starts_with("t-")) {
    json detail = JsonValue(facts, id.c_str(), json::object());
    text = JsonValue(detail, "output", "");
    std::string change = JsonValue(detail, "change", "");
    if (!change.empty()) {
      text += "\n\nRecorded change\n" + change;
    }
    if (text.empty()) {
      for (const auto& [sequence, entry] : Entries(conversation)) {
        if (entry.kind == "tool_result" &&
            "t-" + JsonValue(*entry.message, "tool_call_id", "") == id) {
          text = Text(*entry.message);
        }
      }
    }
  } else {
    for (const auto& [sequence, entry] : Entries(conversation)) {
      if (Visible(entry) && "m-" + std::to_string(sequence) == id) {
        text = Text(*entry.message);
      }
    }
  }
  offset = std::min(offset, text.size());
  size_t end = Utf8BoundaryBefore(text, std::min(text.size(), offset + kPage));
  offset = Utf8BoundaryAfter(text, offset);
  return {{"text", text.substr(offset, end >= offset ? end - offset : 0)},
          {"next", end},
          {"bytes", text.size()},
          {"more", end < text.size()}};
}

json FindHttpExchange(const json& value, const std::string& id) {
  if (const json* exchanges = JsonArray(value, "http")) {
    if (id == "latest" && !exchanges->empty()) return exchanges->back();
    for (const json& exchange : *exchanges) {
      if (JsonValue(exchange, "id", "") == id) return exchange;
    }
  }
  if (value.is_array() || value.is_object()) {
    for (const json& child : value) {
      if (!child.is_structured()) continue;
      json found = FindHttpExchange(child, id);
      if (!found.is_null()) return found;
    }
  }
  return nullptr;
}

json ReadPrivateArtifact(const std::string& path, size_t offset) {
  const auto base = CanonicalAccessPath(UagentDir(kArtifactsDir));
  const std::filesystem::path file_path(path);
  if (path.empty() || CanonicalAccessPath(file_path.parent_path()) != base) {
    return {{"error", "retained body unavailable"}};
  }
  Fd file(open((base / file_path.filename()).c_str(),
               O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  struct stat info{};
  if (!file || fstat(file.Get(), &info) || !S_ISREG(info.st_mode) ||
      info.st_uid != geteuid() || info.st_size < 0) {
    return {{"error", "retained body unavailable"}};
  }
  size_t size = static_cast<size_t>(info.st_size);
  offset = std::min(offset, size);
  constexpr size_t kPage = size_t{16} * 1024;
  // Look ahead so the page boundary can be checked against the next byte.
  std::string text(std::min(size - offset, kPage + 4), '\0');
  ssize_t bytes;
  do {
    bytes =
        pread(file.Get(), text.data(), text.size(), static_cast<off_t>(offset));
  } while (bytes < 0 && errno == EINTR);
  if (bytes < 0) return {{"error", "retained body read failed"}};
  text.resize(static_cast<size_t>(bytes));
  // JSON pages split only at UTF-8 boundaries, never inside a code point.
  const size_t start = Utf8BoundaryAfter(text, 0);
  const size_t end = Utf8BoundaryBefore(text, std::min(kPage, text.size()));
  return {{"text", text.substr(start, end - start)},
          {"next", offset + end},
          {"bytes", size},
          {"more", offset + end < size}};
}

json ConversationExchange(const Conversation& conversation,
                          const std::string& id, size_t offset) {
  json facts =
      JsonValue(conversation.DisplayFacts(), id.c_str(), json::object());
  std::string path = JsonValue(facts, "exchange_path", "");
  if (!path.empty()) return ReadPrivateArtifact(path, offset);
  json exchange = JsonValue(facts, "exchange", json(nullptr));
  if (exchange.is_null()) {
    if (!id.starts_with("t-")) {
      return ConversationDetail(conversation, id, offset);
    }
    exchange = {{"request", nullptr},
                {"response", JsonValue(facts, "output", "")},
                {"complete", false}};
    for (const auto& [sequence, entry] : Entries(conversation)) {
      if (const json* calls = JsonArray(*entry.message, "tool_calls")) {
        for (const json& call : *calls) {
          if ("t-" + JsonValue(call, "id", "") == id) {
            exchange["request"] = JsonValue(call, "function", json::object());
          }
        }
      }
    }
  }
  std::string text = JsonDump(exchange);
  offset = std::min(offset, text.size());
  size_t end = Utf8BoundaryBefore(
      text, std::min(text.size(), offset + size_t{16} * 1024));
  offset = Utf8BoundaryAfter(text, offset);
  return {{"text", text.substr(offset, end - offset)},
          {"next", end},
          {"bytes", text.size()},
          {"more", end < text.size()}};
}
}  // namespace uagent
