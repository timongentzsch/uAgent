// Copyright 2026 Timon Gentzsch
// Reading a saved transcript: pages of whole turns, search, and a node's
// full text by its id.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/node.h"
#include "include/agent/transcript.h"
#include "include/core/limits.h"
#include "include/core/private_store.h"
#include "include/core/strings.h"

namespace uagent {
namespace {
using Entry = TranscriptEntry;
const json kNone = json::object();

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
    entries.try_emplace(ids[i], Entry{&conversation.At(i),
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

// `nodes` with the work and group nodes they sit under, each before its
// first node.
std::vector<json> WithStructure(
    std::vector<json> nodes, const json& facts,
    const std::map<std::string, const json*>& summaries) {
  std::set<std::string> held;
  for (const json& node : nodes) held.insert(JsonValue(node, "id", ""));
  std::vector<json> out;
  std::function<void(const std::string&)> ensure = [&](const std::string& id) {
    if (!(id.starts_with("w-") || id.starts_with("g-")) ||
        !held.insert(id).second) {
      return;
    }
    const json& recorded = JsonValue(facts, id.c_str(), kNone);
    const auto summary = summaries.find(id.substr(2));
    json node = id.starts_with("g-")
                    ? GroupNode(id, recorded)
                    : WorkNode(id.substr(2), recorded,
                               summary == summaries.end()
                                   ? nullptr
                                   : JsonObject(*summary->second, "summary"));
    ensure(JsonValue(node, "parent", ""));
    out.push_back(std::move(node));
  };
  for (json& node : nodes) {
    ensure(JsonValue(node, "parent", ""));
    out.push_back(std::move(node));
  }
  return out;
}
}  // namespace

TranscriptView::TranscriptView(const Conversation& conversation)
    : conversation_(conversation) {}

const std::map<uint64_t, TranscriptEntry>& TranscriptView::Index() const {
  std::call_once(indexed_, [this] {
    entries_ = Entries(conversation_);
    for (const auto& [sequence, entry] : entries_) {
      if (entry.kind == "turn_summary") {
        summaries_[JsonValue(*entry.message, "turn_root", "")] = entry.message;
      }
    }
  });
  return entries_;
}

json TranscriptView::Page(uint64_t before) const {
  const json& facts = conversation_.DisplayFacts();
  const auto& entries = Index();
  // Newest first, so the bounds keep the latest turns, counting the work and
  // group nodes they sit under; a call's result is met before the call and
  // keeps its node.
  std::vector<json> page, made;
  std::set<std::string> ids;
  size_t bytes = 0, whole = 0;
  bool more = false;
  for (auto it = std::make_reverse_iterator(before ? entries.lower_bound(before)
                                                   : entries.end());
       it != entries.rend() && !more; ++it) {
    made.clear();
    EntryNodes(conversation_, it->first, it->second, true, made);
    for (auto node = made.rbegin(); node != made.rend() && !more; ++node) {
      if (!ids.insert(JsonValue(*node, "id", "")).second) continue;
      if (const std::string parent = JsonValue(*node, "parent", "");
          parent.starts_with("w-") || parent.starts_with("g-")) {
        ids.insert(parent);
      }
      bytes += JsonDump(*node).size();
      more = ids.size() > kViewNodes || bytes > kViewBytes;
      if (more) break;
      page.push_back(std::move(*node));
      if (page.back()["kind"] == "turn") whole = page.size();
    }
  }
  // The page ends on a whole turn, unless one turn is larger than a page.
  if (more && whole) page.resize(whole);
  std::reverse(page.begin(), page.end());
  page = WithStructure(std::move(page), facts, summaries_);
  const auto first = std::ranges::find_if(
      page, [](const json& node) { return node.contains("sequence"); });
  const json oldest = first == page.end() ? json(0) : (*first)["sequence"];
  return {
      {"fork", JsonValue(facts, "fork-origin", json(nullptr))},
      {"blocks", std::move(page)},
      {"before", oldest},
      {"more", more},
      {"dropped_segments", conversation_.DroppedSegments()},
      {"retention", "Full retained content; missing facts are not inferred."}};
}

json TranscriptView::Detail(const std::string& id, size_t offset) const {
  const json& facts = conversation_.DisplayFacts();
  std::string text;
  if (id.starts_with("t-")) {
    const json& detail = JsonValue(facts, id.c_str(), kNone);
    // A diff stored whole is the node in full.
    if (detail.contains("change_path")) {
      return ReadPrivateArtifact(JsonValue(detail, "change_path", ""), offset);
    }
    text = JsonValue(detail, "output", "");
    std::string change = JsonValue(detail, "change", "");
    if (!change.empty()) {
      text += "\n\nRecorded change\n" + change;
    }
    return PageText(text, offset);
  }
  std::vector<json> made;
  for (const auto& [sequence, entry] : Index()) {
    made.clear();
    EntryNodes(conversation_, sequence, entry, false, made);
    for (const json& node : made) {
      if (JsonValue(node, "id", "") == id) {
        text = EntryText(facts, sequence, *entry.message,
                         JsonValue(node, "kind", "") == "thinking");
      }
    }
  }
  return PageText(text, offset);
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

json LatestHttp(const json& facts) {
  const json* latest = JsonObject(facts, "http-latest");
  return latest ? JsonValue(*latest, "http", json::array()) : json::array();
}

json PageText(const std::string& text, size_t offset) {
  offset = std::min(offset, text.size());
  const size_t end =
      Utf8BoundaryBefore(text, std::min(text.size(), offset + kTextPageBytes));
  offset = Utf8BoundaryAfter(text, offset);
  return {{"text", text.substr(offset, end >= offset ? end - offset : 0)},
          {"next", end},
          {"bytes", text.size()},
          {"more", end < text.size()}};
}

json TranscriptView::Exchange(const std::string& id, size_t offset) const {
  const json& facts = conversation_.DisplayFacts();
  const json& detail = JsonValue(facts, id.c_str(), kNone);
  std::string path = JsonValue(detail, "exchange_path", "");
  if (!path.empty()) return ReadPrivateArtifact(path, offset);
  json exchange = JsonValue(detail, "exchange", json(nullptr));
  if (exchange.is_null()) {
    if (!id.starts_with("t-")) return Detail(id, offset);
    exchange = {{"request", nullptr},
                {"response", JsonValue(detail, "output", "")},
                {"complete", false}};
    const std::string call_id = JsonValue(detail, "call_id", "");
    for (const auto& [sequence, entry] : Index()) {
      const json* calls = JsonArray(*entry.message, "tool_calls");
      if (!calls) continue;
      for (const json& call : *calls) {
        if (!call_id.empty() && JsonValue(call, "id", "") == call_id) {
          exchange["request"] = JsonValue(call, "function", json::object());
        }
      }
    }
  }
  return PageText(JsonDump(exchange), offset);
}

json TranscriptView::Search(const std::string& query, size_t limit) const {
  constexpr size_t kSnippetChars = 200;
  json hits = json::array();
  const std::string needle = AsciiLower(query);
  if (needle.empty()) return hits;
  const json& facts = conversation_.DisplayFacts();
  std::vector<json> made;
  for (const auto& [sequence, entry] : Index()) {
    if (hits.size() >= limit) break;
    if (entry.kind != "user" && entry.kind != "assistant" &&
        entry.kind != "tool_result") {
      continue;
    }
    const std::string text = EntryText(facts, sequence, *entry.message);
    const std::string folded = AsciiLower(text);
    const size_t first = folded.find(needle);
    if (first == std::string::npos) continue;
    size_t count = 0;
    for (size_t at = first; at != std::string::npos;
         at = folded.find(needle, at + needle.size())) {
      ++count;
    }
    // Close before the match, so a reader who sees one line sees it.
    constexpr size_t kLead = 40;
    size_t start = first > kLead ? first - kLead : 0;
    // It starts on a word where one starts before the match, and on a
    // character always.
    if (start > 0) {
      const size_t space = text.find_first_of(" \t\n\r", start - 1);
      if (space < first) start = space + 1;
    }
    while (start > 0 &&
           (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) {
      --start;
    }
    std::string snippet = Utf8Prefix(text.substr(start), kSnippetChars);
    std::ranges::replace(snippet, '\n', ' ');
    std::ranges::replace(snippet, '\r', ' ');
    // The hit is the node the text is drawn in.
    made.clear();
    EntryNodes(conversation_, sequence, entry, false, made);
    const json& node = made.empty() ? kNone : made.back();
    hits.push_back({{"id", JsonValue(node, "id", "")},
                    {"sequence", sequence},
                    {"kind", JsonValue(node, "kind", "")},
                    {"count", count},
                    {"snippet", (start ? "…" : "") + snippet}});
  }
  return hits;
}

json ConversationView(const Conversation& conversation, uint64_t before) {
  return TranscriptView(conversation).Page(before);
}

}  // namespace uagent
