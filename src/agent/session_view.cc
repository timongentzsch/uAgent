// Copyright 2026 Timon Gentzsch

#include "include/agent/session_view.h"

#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "include/agent/protocol.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/strings.h"

namespace uagent {
namespace {
constexpr size_t kViewBytes = size_t{384} * 1024;
constexpr size_t kViewBlocks = 64;
using Entry = TranscriptEntry;
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

json TextPage(const std::string& text, size_t offset) {
  offset = std::min(offset, text.size());
  const size_t end =
      Utf8BoundaryBefore(text, std::min(text.size(), offset + KiB(16)));
  offset = Utf8BoundaryAfter(text, offset);
  return {{"text", text.substr(offset, end >= offset ? end - offset : 0)},
          {"next", end},
          {"bytes", text.size()},
          {"more", end < text.size()}};
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

// Scans a double-quoted segment with backslash escapes. `pos` starts on the
// opening quote and ends past the closing one; false when unterminated.
bool ScanQuotedSegment(std::string_view line, size_t& pos) {
  if (pos >= line.size() || line[pos] != '"') return false;
  ++pos;
  while (pos < line.size()) {
    if (line[pos] == '\\') {
      pos += 2;
      continue;
    }
    if (line[pos] == '"') {
      ++pos;
      return true;
    }
    ++pos;
  }
  return false;
}

// One `- path "..."` reference line as AttachmentContent writes it, with
// the optional ` (from tool call "...")` suffix.
bool IsAttachmentReference(std::string_view line) {
  constexpr std::string_view kPrefix = "- path ";
  constexpr std::string_view kFrom = " (from tool call ";
  if (!line.starts_with(kPrefix)) return false;
  size_t pos = kPrefix.size();
  if (!ScanQuotedSegment(line, pos)) return false;
  if (pos == line.size()) return true;
  if (!line.substr(pos).starts_with(kFrom)) return false;
  pos += kFrom.size();
  if (!ScanQuotedSegment(line, pos)) return false;
  if (pos >= line.size() || line[pos] != ')') return false;
  ++pos;
  return pos == line.size();
}

bool IsBlankLine(std::string_view line) {
  return line.find_first_not_of(" \t\r") == std::string_view::npos;
}

// Stored user text keeps the "Attached:" trailer for the model; readers see
// the text they typed, with the attachments shown from their own records.
std::string DisplayText(const json& message) {
  std::string text = Text(message);
  if (JsonValue(message, "role", "") == "tool") {
    return StripModelHints(std::move(text));
  }
  const json* content = JsonArray(message, "content");
  if (!content) return text;
  for (const json& part : *content) {
    if (JsonValue(part, "type", "") == "attachment") {
      return StripAttachedTrailer(text);
    }
  }
  return text;
}
}  // namespace

const json* MergeDisplayBlock(json& view, const json& block) {
  json& blocks = view["blocks"];
  if (!blocks.is_array()) blocks = json::array();
  const std::string id = JsonValue(block, "id", "");
  auto found = std::find_if(
      blocks.begin(), blocks.end(),
      [&](const json& item) { return JsonValue(item, "id", "") == id; });
  if (found == blocks.end()) {
    // Retained rows carry their sequence: insert in position so a late
    // arrival (replay, pre-facts emit) can never strand older content
    // after newer rows. Sequence-less rows keep their relative order.
    found = blocks.end();
    if (block.contains("sequence") && block["sequence"].is_number()) {
      const auto sequence = block["sequence"].get<uint64_t>();
      found = std::find_if(blocks.begin(), blocks.end(), [&](const json& item) {
        return item.contains("sequence") && item["sequence"].is_number() &&
               item["sequence"].get<uint64_t>() > sequence;
      });
    }
    found = blocks.insert(found, block);
  } else {
    // One row per id, whichever side of it arrives: a tool call's fields and
    // its result's fill one row. A retained row has finished streaming, and
    // a shorter preview of the same revision never replaces streamed text.
    json merged = *found;
    merged.update(block);
    if (block.contains("sequence")) merged.erase("streaming");
    for (const auto& [field, bytes, revision, complete] :
         {std::tuple{"text", "text_bytes", "content_revision",
                     "content_complete"},
          std::tuple{"reasoning", "reasoning_bytes", "reasoning_revision",
                     "reasoning_complete"}}) {
      if (JsonValue(*found, revision, uint64_t{0}) ==
              JsonValue(block, revision, uint64_t{0}) &&
          JsonValue(*found, bytes, size_t{0}) >
              JsonValue(block, bytes, size_t{0})) {
        merged[field] = (*found)[field];
        merged[bytes] = (*found)[bytes];
        merged[complete] = JsonValue(*found, complete, false);
        if (std::string_view(field) == "text") {
          merged["truncated"] = JsonValue(*found, "truncated", false);
        }
      }
    }
    *found = std::move(merged);
  }
  const size_t kept = static_cast<size_t>(found - blocks.begin());
  size_t bytes = JsonEstimatedBytes(blocks);
  size_t dropped = 0;
  while (dropped < blocks.size() &&
         (blocks.size() - dropped > kViewBlocks || bytes > kViewBytes)) {
    bytes -= std::min(bytes, JsonEstimatedBytes(blocks[dropped]));
    ++dropped;
  }
  if (dropped) {
    blocks.erase(blocks.begin(),
                 blocks.begin() + static_cast<std::ptrdiff_t>(dropped));
    view["more"] = true;
  }
  if (!blocks.empty()) {
    view["before"] = JsonValue(blocks.front(), "sequence", uint64_t{0});
  }
  return kept >= dropped ? &blocks[kept - dropped] : nullptr;
}

namespace {
json* FindBlock(json& state, const std::string& id) {
  json& blocks = state["view"]["blocks"];
  if (id.empty() || !blocks.is_array()) return nullptr;
  auto found = std::find_if(blocks.begin(), blocks.end(), [&](json& block) {
    return JsonValue(block, "id", "") == id;
  });
  return found == blocks.end() ? nullptr : &*found;
}

// A live tool row, keyed like the retained one DisplayBlock builds for the
// same call, so the result, the saved message and a reload all land on it.
json LiveToolRow(const json& data) {
  const json arguments = JsonValue(data, "arguments", json(""));
  return {{"id", JsonValue(data, "detail_id", "")},
          {"kind", "tool_result"},
          {"call_id", JsonValue(data, "call_id", "")},
          {"response_id", JsonValue(data, "response_id", "")},
          {"occurrence_id", JsonValue(data, "occurrence_id", "")},
          {"detail_id", JsonValue(data, "detail_id", "")},
          {"name", JsonValue(data, "name", "")},
          {"arguments",
           Utf8Trunc(arguments.is_string() ? arguments.get<std::string>()
                                           : JsonDump(arguments),
                     1024)},
          {"activity", JsonValue(data, "activity", json::object())},
          {"view", JsonValue(data, "view", json(nullptr))},
          {"status", "running"},
          {"text", ""}};
}

json LiveToolResult(const json& data) {
  // The row shows the output as a person reads it, without model hints.
  const std::string text = StripModelHints(JsonValue(data, "result", ""));
  json row = {{"id", JsonValue(data, "detail_id", "")},
              {"kind", "tool_result"},
              {"activity", JsonValue(data, "activity", json::object())},
              {"status", JsonValue(data, "completion_status",
                                   JsonValue(data, "status", "complete"))},
              {"duration_ms", JsonValue(data, "duration_ms", 0.0)},
              {"text", Utf8Trunc(text, kPreviewChars - 3)},
              {"truncated", text.size() > kPreviewChars}};
  if (data.contains("parts")) row["parts"] = data["parts"];
  return row;
}
}  // namespace

bool ApplySessionEvent(json& state, const std::string& type, const json& data,
                       json* patch) {
  // A patch is what a client holding the same view must apply to match it:
  // the whole changed block, or an append to one of its streamed fields.
  const auto changed = [&](const json* block) {
    if (patch && block) *patch = {{"kind", "block"}, {"block", *block}};
  };
  if (type == "usage.updated") {
    state["usage"] = data["usage"];
    if (data.contains("context_tokens")) {
      state["context_tokens"] = data["context_tokens"];
    }
    if (data.contains("statistics")) state["statistics"] = data["statistics"];
  } else if (type == "response.started") {
    const std::string response_id = JsonValue(data, "response_id", "");
    if (!response_id.empty()) {
      changed(
          MergeDisplayBlock(state["view"], {{"id", response_id},
                                            {"row_id", response_id},
                                            {"response_id", response_id},
                                            {"kind", "assistant"},
                                            {"streaming", true},
                                            {"text", ""},
                                            {"text_bytes", 0},
                                            {"content_revision", 1},
                                            {"content_complete", false},
                                            {"reasoning", ""},
                                            {"reasoning_bytes", 0},
                                            {"reasoning_revision", 1},
                                            {"reasoning_complete", false}}));
    }
  } else if (type == "response.answer.delta" ||
             type == "response.reasoning.delta") {
    // Deltas without a response id have no started block to extend (the
    // started branch above requires one). The full block still arrives via
    // message.changed, so skipping here loses nothing.
    const std::string response_id = JsonValue(data, "response_id", "");
    json* found = FindBlock(state, response_id);
    const bool answer = type == "response.answer.delta";
    if (!found ||
        JsonValue(*found, answer ? "content_complete" : "reasoning_complete",
                  false)) {
      return true;
    }
    const char* field = answer ? "text" : "reasoning";
    const char* bytes = answer ? "text_bytes" : "reasoning_bytes";
    const std::string current = JsonValue(*found, field, "");
    const std::string delta = JsonValue(data, "text", "");
    std::string added = delta;
    if (!answer && JsonValue(data, "reset", false)) {
      (*found)[field] = delta;
      (*found)[bytes] = delta.size();
      changed(found);
      return true;
    }
    if (data.contains("offset")) {
      const size_t offset = JsonValue(data, "offset", size_t{0});
      // A checkpoint may already contain this delta. Only append the
      // still-missing suffix when its overlapping bytes agree; a gap or
      // conflicting revision waits for the next complete block.
      if (offset > current.size()) return true;
      const size_t overlap = std::min(delta.size(), current.size() - offset);
      if (current.compare(offset, overlap, delta, 0, overlap) != 0) {
        return true;
      }
      added = delta.substr(overlap);
    }
    if (added.empty()) return true;
    (*found)[field] = current + added;
    (*found)[bytes] = current.size() + added.size();
    if (patch) {
      *patch = {{"kind", "block"},
                {"id", response_id},
                {"append", {{field, std::move(added)}}}};
    }
  } else if (type == "response.finished") {
    if (json* found = FindBlock(state, JsonValue(data, "response_id", ""))) {
      (*found)["streaming"] = false;
      if (patch) {
        *patch = {{"kind", "block"},
                  {"id", (*found)["id"]},
                  {"set", {{"streaming", false}}}};
      }
    }
  } else if (type == "tool.call" || type == "tool.result") {
    if (JsonValue(data, "detail_id", "").empty()) return true;
    changed(MergeDisplayBlock(state["view"], type == "tool.call"
                                                 ? LiveToolRow(data)
                                                 : LiveToolResult(data)));
  } else if (type == "message.changed") {
    changed(MergeDisplayBlock(state["view"], data["block"]));
  } else if (type == "activities.changed") {
    state["activities"] = data["activities"];
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
// Receipt facts live under the call's detail id. A message whose id
// metadata was never recorded still carries the call id, so scan by it
// before admitting the receipt is gone. Message-metadata entries carry
// call ids too but never a receipt name, so they cannot self-match.
const json* FindToolFacts(const json& facts, const std::string& detail_id,
                          const std::string& call_id,
                          std::string* resolved_id) {
  const auto direct = facts.find(detail_id);
  if (direct != facts.end() && direct->is_object() && !direct->empty()) {
    if (resolved_id) *resolved_id = detail_id;
    return &*direct;
  }
  for (auto it = facts.begin(); it != facts.end(); ++it) {
    if (!call_id.empty() && it->is_object() &&
        JsonValue(*it, "call_id", "") == call_id && it->contains("name")) {
      if (resolved_id) *resolved_id = it.key();
      return &*it;
    }
  }
  return nullptr;
}

// One message's rows: the message itself, and for an assistant message one
// row per tool call, keyed by the call's detail id like its result row, so
// the two join into one wherever they meet.
void DisplayBlocks(const Conversation& conversation, uint64_t sequence,
                   const Entry& entry, std::vector<json>& out) {
  const json& facts = conversation.DisplayFacts();
  std::string id = "m-" + std::to_string(sequence);
  const json& message = *entry.message;
  std::string text = DisplayText(message);
  json block = {{"id", id},
                {"sequence", sequence},
                {"kind", entry.kind},
                {"text", Utf8Trunc(text, kPreviewChars - 3)},
                {"truncated", text.size() > kPreviewChars}};
  json metadata = JsonValue(facts, id.c_str(), json::object());
  for (const char* key : {"time",
                          "incoming",
                          "activity_id",
                          "agent_id",
                          "command",
                          "status",
                          "request_id",
                          "route",
                          "duration_ms",
                          "ttft_ms",
                          "usage",
                          "usage_reported",
                          "tokens_per_second",
                          "turn_root",
                          "reply_to",
                          "reply_excerpt",
                          "http",
                          "files",
                          "summary",
                          "deliveries",
                          "activity",
                          "origin",
                          "source_call_ids",
                          "compaction",
                          "memory",
                          "view",
                          "parts",
                          "response_id",
                          "content_revision",
                          "content_complete",
                          "text_bytes",
                          "reasoning_revision",
                          "reasoning_complete",
                          "reasoning_bytes",
                          "call_id",
                          "occurrence_id",
                          "detail_id"}) {
    if (metadata.contains(key)) block[key] = metadata[key];
  }
  block["retained_text_bytes"] = text.size();
  block["text_bytes"] = JsonValue(block, "text", "").size();
  block["content_complete"] = !JsonValue(block, "truncated", false);
  std::vector<json> rows;
  if (entry.kind == "assistant") {
    const std::string response_id = JsonValue(metadata, "response_id", "");
    // The live row streamed under the response id; the retained one is the
    // same row.
    if (!response_id.empty()) {
      block["id"] = response_id;
      block["row_id"] = response_id;
    }
    if (!block.contains("content_revision")) block["content_revision"] = 1;
    if (!block.contains("content_complete")) {
      block["content_complete"] = text.size() <= 4096;
    }
    if (!block.contains("text_bytes")) block["text_bytes"] = text.size();
    std::string reasoning = JsonValue(metadata, "reasoning", "");
    if (reasoning.empty()) {
      reasoning = JsonValue(message, "reasoning_content",
                            JsonValue(message, "reasoning", ""));
    }
    block["reasoning"] = Utf8Trunc(reasoning, kPreviewChars - 3);
    block["reasoning_available"] = !reasoning.empty();
    if (!block.contains("reasoning_revision")) block["reasoning_revision"] = 1;
    block["retained_reasoning_bytes"] = reasoning.size();
    block["reasoning_bytes"] = JsonValue(block, "reasoning", "").size();
    block["reasoning_complete"] = reasoning.size() <= kPreviewChars;
    if (const json* calls = JsonArray(message, "tool_calls")) {
      for (const json& call : *calls) {
        if (rows.size() >= kMaxToolsPerMessage) break;
        std::string call_id = JsonValue(call, "id", "");
        std::string detail_id = DetailId(response_id, call_id);
        const json* found =
            FindToolFacts(facts, detail_id, call_id, &detail_id);
        const json detail = found ? *found : json::object();
        json function = JsonValue(call, "function", json::object());
        json row = {{"id", detail_id},
                    {"sequence", sequence},
                    {"kind", "tool_result"},
                    {"call_id", call_id},
                    {"response_id", response_id},
                    {"occurrence_id", OccurrenceId(response_id, call_id)},
                    {"detail_id", detail_id},
                    {"name", Utf8Trunc(JsonValue(function, "name", ""), 128)},
                    {"arguments",
                     Utf8Trunc(JsonValue(function, "arguments", ""), 1024)},
                    {"status", JsonValue(detail, "status", "running")},
                    {"activity", JsonValue(detail, "activity", json::object())},
                    {"text", ""}};
        // Kept receipts replay the exact live row and its view.
        if (detail.contains("call_replay")) {
          row["call_replay"] = detail["call_replay"];
          if (const json* view = JsonObject(detail["call_replay"], "view")) {
            row["view"] = *view;
          }
        }
        if (detail.contains("exchange_path")) {
          row["exchange_path"] = detail["exchange_path"];
        }
        rows.push_back(std::move(row));
      }
    }
  }
  if (entry.kind == "tool_result") {
    std::string call_id = JsonValue(message, "tool_call_id", "");
    std::string detail_id = JsonValue(metadata, "detail_id", "t-" + call_id);
    const json* found = FindToolFacts(facts, detail_id, call_id, &detail_id);
    const json detail = found ? *found : json::object();
    // Retained rows finished by definition: a missing receipt is a gap in
    // history, never live activity (live rows stream separately). Say so
    // explicitly instead of counterfeiting a "running" state.
    const bool receipt_missing = found == nullptr;
    block["id"] = detail_id;
    block["call_id"] = call_id;
    block["activity"] = JsonValue(detail, "activity", json::object());
    block["name"] = JsonValue(detail, "name", "tool");
    block["status"] =
        JsonValue(detail, "status", receipt_missing ? "complete" : "running");
    if (receipt_missing) block["receipt_missing"] = true;
    if (detail.contains("duration_ms")) {
      block["duration_ms"] = detail["duration_ms"];
    }
    block["detail_id"] = detail_id;
    block["change"] = Utf8Trunc(JsonValue(detail, "change", ""), kPreviewChars);
    if (detail.contains("parts")) block["parts"] = detail["parts"];
    // The preview keeps the start of a long result; its end is where a
    // command reports how it went, so rows can show that tail.
    if (text.size() > kPreviewChars) {
      size_t start = text.size() - 512;
      while (start < text.size() &&
             (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) {
        ++start;
      }
      block["tail"] = text.substr(start);
    }
    block["artifact"] = detail.contains("artifact");
    if (detail.contains("result_replay")) {
      block["replay"] = detail["result_replay"];
    }
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
  out.push_back(std::move(block));
  for (json& row : rows) out.push_back(std::move(row));
}
}  // namespace

json LastMessageView(const Conversation& conversation) {
  if (conversation.Empty()) return nullptr;
  const size_t last = conversation.Size() - 1;
  Entry entry{&conversation.At(last),
              MessageKindName(conversation.KindAt(last))};
  if (!Visible(entry)) return nullptr;
  // The message itself; its tool rows arrive live as their calls run.
  std::vector<json> blocks;
  DisplayBlocks(conversation, conversation.DisplayIds()[last], entry, blocks);
  return std::move(blocks.front());
}

TranscriptView::TranscriptView(const Conversation& conversation)
    : conversation_(conversation) {}

const std::map<uint64_t, TranscriptEntry>& TranscriptView::Index() const {
  std::call_once(indexed_, [this] { entries_ = Entries(conversation_); });
  return entries_;
}

json TranscriptView::Page(uint64_t before) const {
  const auto& conversation = conversation_;
  std::vector<json> blocks;
  size_t bytes = 0;
  uint64_t first = 0;
  bool more = false;
  const auto& entries = Index();
  auto end = before ? entries.lower_bound(before) : entries.end();
  // Newest first, so the byte budget keeps the latest rows. A tool result
  // is met before its call and keeps its row; the call fills in its fields.
  std::map<std::string, size_t> rows;
  std::vector<json> message;
  for (auto it = std::make_reverse_iterator(end); it != entries.rend() && !more;
       ++it) {
    const Entry& entry = it->second;
    if (!Visible(entry)) continue;
    message.clear();
    DisplayBlocks(conversation, it->first, entry, message);
    for (auto block = message.rbegin(); block != message.rend(); ++block) {
      const std::string id = JsonValue(*block, "id", "");
      if (auto row = rows.find(id); row != rows.end()) {
        json& into = blocks[row->second];
        for (auto field = block->begin(); field != block->end(); ++field) {
          if (!into.contains(field.key())) into[field.key()] = field.value();
        }
        continue;
      }
      bytes += JsonDump(*block).size();
      if (bytes > kViewBytes || blocks.size() >= kViewBlocks) {
        more = true;
        break;
      }
      first = it->first;
      rows.emplace(id, blocks.size());
      blocks.push_back(std::move(*block));
    }
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

json TranscriptView::Detail(const std::string& id, size_t offset) const {
  const auto& conversation = conversation_;
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
      for (const auto& [sequence, entry] : Index()) {
        if (entry.kind == "tool_result" &&
            "t-" + JsonValue(*entry.message, "tool_call_id", "") == id) {
          text = Text(*entry.message);
        }
      }
    }
  } else if (id.starts_with("m-")) {
    uint64_t sequence = 0;
    const auto [end, error] =
        std::from_chars(id.data() + 2, id.data() + id.size(), sequence);
    if (error == std::errc() && end == id.data() + id.size() &&
        "m-" + std::to_string(sequence) == id) {
      const auto& entries = Index();
      auto entry = entries.find(sequence);
      if (entry != entries.end() && Visible(entry->second)) {
        text = DisplayText(*entry->second.message);
      }
    }
  } else {
    // An assistant row is addressed by its response id.
    for (const auto& [sequence, entry] : Index()) {
      const std::string key = "m-" + std::to_string(sequence);
      if (entry.kind == "assistant" &&
          JsonValue(JsonValue(facts, key.c_str(), json::object()),
                    "response_id", "") == id) {
        text = DisplayText(*entry.message);
      }
    }
  }
  return TextPage(text, offset);
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
  size_t size = 0;
  Fd file =
      OpenOwnedRegular((base / file_path.filename()).string(), SIZE_MAX, &size);
  if (!file) return {{"error", "retained body unavailable"}};
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

json TranscriptView::Exchange(const std::string& id, size_t offset) const {
  const auto& conversation = conversation_;
  json facts =
      JsonValue(conversation.DisplayFacts(), id.c_str(), json::object());
  std::string path = JsonValue(facts, "exchange_path", "");
  if (!path.empty()) return ReadPrivateArtifact(path, offset);
  json exchange = JsonValue(facts, "exchange", json(nullptr));
  if (exchange.is_null()) {
    if (!id.starts_with("t-")) {
      return Detail(id, offset);
    }
    exchange = {{"request", nullptr},
                {"response", JsonValue(facts, "output", "")},
                {"complete", false}};
    for (const auto& [sequence, entry] : Index()) {
      if (const json* calls = JsonArray(*entry.message, "tool_calls")) {
        for (const json& call : *calls) {
          if ("t-" + JsonValue(call, "id", "") == id) {
            exchange["request"] = JsonValue(call, "function", json::object());
          }
        }
      }
    }
  }
  return TextPage(JsonDump(exchange), offset);
}
json ConversationView(const Conversation& conversation, uint64_t before) {
  return TranscriptView(conversation).Page(before);
}
json ConversationDetail(const Conversation& conversation, const std::string& id,
                        size_t offset) {
  return TranscriptView(conversation).Detail(id, offset);
}
json ConversationExchange(const Conversation& conversation,
                          const std::string& id, size_t offset) {
  return TranscriptView(conversation).Exchange(id, offset);
}

std::string StripModelHints(std::string text) {
  for (std::string_view hint :
       {"[running] activity ", "[started] subagent id ", "[detached] pid "}) {
    if (text.starts_with(hint)) {
      const size_t end = text.find('\n');
      text.erase(0, end == std::string::npos ? text.size() : end + 1);
      break;
    }
  }
  const size_t line = text.rfind("\n[collaborator ");
  if (line != std::string::npos && text.back() == ']' &&
      text.find('\n', line + 1) == std::string::npos) {
    text.resize(line);
  }
  return text;
}

std::string StripAttachedTrailer(const std::string& text) {
  constexpr std::string_view kMarker = "\n\nAttached:\n";
  const size_t marker = text.rfind(kMarker);
  if (marker == std::string::npos) return text;
  size_t pos = marker + kMarker.size();
  bool referenced = false;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    const size_t stop = end == std::string::npos ? text.size() : end;
    const std::string_view line(text.data() + pos, stop - pos);
    if (IsBlankLine(line)) {
      // A trailing whitespace tail is formatting, not user text.
      if (text.find_first_not_of(" \t\r\n", pos) == std::string::npos) {
        break;
      }
      return text;
    }
    if (!IsAttachmentReference(line)) return text;
    referenced = true;
    pos = end == std::string::npos ? text.size() : end + 1;
  }
  if (!referenced) return text;
  return text.substr(0, marker);
}

}  // namespace uagent
