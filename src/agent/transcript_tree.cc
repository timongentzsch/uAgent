// Copyright 2026 Timon Gentzsch
// The transcript's nodes, built from what was recorded: the same nodes for
// a live view and a saved page.

#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/agent/node.h"
#include "include/agent/protocol.h"
#include "include/agent/transcript.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/core/style.h"
#include "include/media/attachments.h"
#include "include/tools/tool.h"

namespace uagent {
namespace {
const json kNone = json::object();

// Copies the fields of `record` a node carries as they are.
void Keep(json& node, const json& record,
          std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    if (record.contains(key)) node[key] = record[key];
  }
}

const json& Metadata(const json& facts, uint64_t sequence) {
  const auto found = facts.find("m-" + std::to_string(sequence));
  return found != facts.end() ? *found : kNone;
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

// Receipt facts live under the call's node id. A message whose id metadata
// was never recorded still carries the call id, so scan by it before
// admitting the receipt is gone. Message-metadata entries carry call ids too
// but never a receipt name, so they cannot self-match.
const json* FindToolFacts(const json& facts, std::string& id,
                          const std::string& call_id) {
  const auto direct = facts.find(id);
  if (direct != facts.end() && direct->is_object() && !direct->empty()) {
    return &*direct;
  }
  for (auto it = facts.begin(); it != facts.end(); ++it) {
    if (!call_id.empty() && it->is_object() &&
        JsonValue(*it, "call_id", "") == call_id && it->contains("name")) {
      id = it.key();
      return &*it;
    }
  }
  return nullptr;
}

// A tool result's facts, and its node id through `id`.
const json* ResultRecord(const json& facts, uint64_t sequence,
                         const json& message, std::string& id) {
  const std::string call_id = JsonValue(message, "tool_call_id", "");
  id = JsonValue(Metadata(facts, sequence), "detail_id", "t-" + call_id);
  return FindToolFacts(facts, id, call_id);
}

bool IsFile(const json& part) { return JsonValue(part, "kind", "") == "file"; }

// The verb a run of same-intent calls folds under; empty for intents whose
// calls always keep their own rows.
std::string GroupVerb(const std::string& intent) {
  return intent == "explore"    ? "Explored"
         : intent == "research" ? "Researched"
         : intent == "verify"   ? "Verified"
         : intent == "edit"     ? "Edited"
                                : "";
}
}  // namespace

json MakeNode(std::string id, std::string_view parent, std::string_view kind,
              json fields) {
  fields["id"] = std::move(id);
  fields["parent"] = parent.empty() ? json(nullptr) : json(parent);
  fields["kind"] = kind;
  return fields;
}

void SetBody(json& node, std::string_view kind, const std::string& text,
             bool truncated) {
  if (Blank(text)) return;
  node["body"] = {{"kind", kind}, {"text", Utf8Trunc(text, kPreviewChars - 3)}};
  if (truncated || text.size() > kPreviewChars) {
    node["body"]["truncated"] = true;
  }
}

bool Blank(std::string_view text) {
  return text.find_first_not_of(" \t\r\n") == std::string_view::npos;
}
std::string WorkId(std::string_view message) {
  return message.empty() ? "" : "w-" + std::string(message);
}
std::string ThinkingId(std::string_view message) {
  return std::string(message) + ":thinking";
}
json ThinkingNode(std::string_view message, std::string_view work,
                  json fields) {
  fields["headline"] = {{"verb", "Thinking"}, {"target", ""}};
  return MakeNode(ThinkingId(message), work, "thinking", std::move(fields));
}

json CallHeadline(const json& view, std::string_view state,
                  const std::string& sent) {
  const json* verb = JsonArray(view, "verb");
  if (!verb || verb->size() != 2 || !(*verb)[0].is_string() ||
      !(*verb)[1].is_string()) {
    return {{"verb", ""}, {"target", sent}};
  }
  return {{"verb", (*verb)[state == "running" ? 0 : 1]},
          {"target", JsonValue(view, "target", "")}};
}

json CallNode(const json& record, std::string_view work) {
  const std::string state = JsonValue(record, "state", "running");
  const json activity = JsonValue(record, "activity", kNone);
  const json view = JsonValue(record, "view", json(nullptr));
  const std::string group = JsonValue(record, "group", "");
  json node = MakeNode(
      JsonValue(record, "detail_id", ""), group.empty() ? work : group, "call",
      {{"state", state},
       {"headline", JsonValue(record, "headline",
                              CallHeadline(view, state,
                                           JsonValue(activity, "label",
                                                     JsonValue(record, "name",
                                                               "tool"))))}});
  // A change is told by its diff, kept whole up to its own bound.
  if (const std::string change = JsonValue(record, "change", "");
      state == "ok" && !change.empty()) {
    node["body"] = {{"kind", "diff"},
                    {"text", change},
                    {"truncated", record.contains("change_path")}};
  } else {
    SetBody(node, JsonValue(record, "body_kind", "text"),
            JsonValue(record, "output", ""),
            JsonValue(record, "truncated", false));
    if (node.contains("body")) Keep(node["body"], record, {"tail"});
  }
  if (const json* input = JsonArray(view, "input"); input && !input->empty()) {
    node["input"] = *input;
  }
  // Files it handed over are a node of their own (FilesNode).
  if (const json* parts = JsonArray(record, "parts")) {
    json kept = json::array();
    for (const json& part : *parts) {
      if (!IsFile(part)) kept.push_back(part);
    }
    if (!kept.empty()) node["parts"] = std::move(kept);
  }
  Keep(node, record, {"result"});
  return node;
}

json FilesNode(const json& record, std::string_view turn) {
  const json* parts = JsonArray(record, "parts");
  if (!parts) return nullptr;
  json files = json::array();
  for (const json& part : *parts) {
    if (IsFile(part)) files.push_back(part);
  }
  if (files.empty()) return nullptr;
  return MakeNode(JsonValue(record, "detail_id", "") + ":files", turn,
                  "message",
                  {{"role", "assistant"}, {"parts", std::move(files)}});
}

json WorkNode(const std::string& message, const json& facts) {
  const auto count = [&](const char* key) {
    return JsonValue(facts, key, int64_t{0});
  };
  const int64_t steps = count("items"), edited = count("edited"),
                errors = count("errors");
  const std::string outcome = JsonValue(facts, "outcome", "");
  const bool done = !outcome.empty();
  return MakeNode(
      WorkId(message), message, "work",
      {{"state", !done                      ? "running"
                 : outcome == "complete"    ? "ok"
                 : outcome == "interrupted" ? "cancelled"
                                            : "failed"},
       {"headline",
        {{"verb", done ? "Worked" : "Working"},
         {"target", JoinDot({steps ? Plural(steps, "step") : "",
                             edited ? "edited " + Plural(edited, "file") : "",
                             errors ? Plural(errors, "error") : ""})}}},
       {"result",
        {{"word",
          done ? FmtDuration(JsonValue(facts, "duration_ms", 0.0) / 1000) : ""},
         {"items", steps},
         {"unit", "step"},
         {"edited", edited},
         {"errors", errors}}}});
}

std::vector<json> GroupCalls(const std::vector<json>& activities) {
  std::vector<json> groups;
  for (size_t begin = 0; begin < activities.size();) {
    const std::string intent = JsonValue(activities[begin], "category", "");
    const std::string verb = GroupVerb(intent);
    auto eligible = [&](size_t i) {
      return !verb.empty() &&
             JsonValue(activities[i], "category", "") == intent &&
             JsonValue(activities[i], "groupable", false) &&
             JsonValue(activities[i], "state", "") == "ok";
    };
    size_t end = begin + 1;
    while (eligible(begin) && end < activities.size() && eligible(end)) ++end;
    if (end - begin > 1) {
      json members = json::array();
      for (size_t i = begin; i < end; ++i) {
        members.push_back(JsonValue(activities[i], "detail_id", ""));
      }
      groups.push_back({{"id", "g-" + members[0].get<std::string>()},
                        {"verb", verb},
                        {"members", std::move(members)}});
    }
    begin = end;
  }
  return groups;
}

json GroupNode(const std::string& id, const json& group) {
  const int64_t calls = JsonValue(group, "calls", int64_t{0});
  return MakeNode(id, JsonValue(group, "work", ""), "group",
                  {{"state", "ok"},
                   {"headline",
                    {{"verb", JsonValue(group, "verb", "")},
                     {"target", Plural(calls, "call")}}},
                   {"result", {{"items", calls}, {"unit", "call"}}}});
}

json EntryNode(const json& entry) {
  const std::string kind = JsonValue(entry, "kind", "activity");
  const std::string state = JsonValue(entry, "state", "ok");
  json node =
      MakeNode(JsonValue(entry, "id", ""), JsonValue(entry, "turn_root", ""),
               kind == "turn_summary" ? "footer" : "notice");
  Keep(node, entry, {"sequence", "time", "incoming", "summary", "compaction"});
  if (kind == "turn_summary") return node;
  node["state"] = state;
  if (kind == "compaction") {
    const json facts = JsonValue(entry, "compaction", kNone);
    const auto count = [&](const char* key) {
      return std::to_string(JsonValue(facts, key, int64_t{0}));
    };
    node["headline"] = {{"verb", "Compacted"}, {"target", ""}};
    node["result"] = {
        {"word",
         JoinDot({count("messages_before") + " → " + count("messages_after") +
                      " messages",
                  FmtDuration(JsonValue(facts, "duration_ms", 0.0) / 1000)})}};
    return node;
  }
  const json activity = JsonValue(entry, "activity", kNone);
  node["headline"] = CallHeadline(JsonValue(entry, "view", json(nullptr)),
                                  state, JsonValue(activity, "label", ""));
  SetBody(node, JsonValue(entry, "body_kind", "text"),
          JsonValue(entry, "text", ""));
  Keep(node, entry,
       {"result", "routine", "parts", "memory", "activity_id", "agent_id",
        "command"});
  return node;
}

std::string EntryText(const json& facts, uint64_t sequence, const json& message,
                      bool reasoning) {
  if (reasoning) {
    std::string text = JsonValue(Metadata(facts, sequence), "reasoning", "");
    return text.empty() ? JsonValue(message, "reasoning_content",
                                    JsonValue(message, "reasoning", ""))
                        : text;
  }
  if (JsonValue(message, "role", "") == "tool") {
    std::string id;
    const json* found = ResultRecord(facts, sequence, message, id);
    return found ? JsonValue(*found, "output", "") : std::string();
  }
  if (JsonValue(message, "kind", "") == "activity") {
    return JsonValue(message, "text", "");
  }
  const json* content = JsonArray(message, "content");
  const bool attached =
      content && std::ranges::any_of(*content, [](const json& part) {
        return JsonValue(part, "type", "") == "attachment";
      });
  return attached ? StripAttachedTrailer(ContentText(message, ""))
                  : ContentText(message, "");
}

void EntryNodes(const Conversation& conversation, uint64_t sequence,
                const TranscriptEntry& entry, bool calls,
                std::vector<json>& out) {
  const json& facts = conversation.DisplayFacts();
  const json& message = *entry.message;
  if (entry.kind == "activity" || entry.kind == "turn_summary" ||
      entry.kind == "compaction") {
    out.push_back(EntryNode(message));
  }
  if (entry.kind != "user" && entry.kind != "attachment" &&
      entry.kind != "assistant" && entry.kind != "tool_result") {
    return;
  }
  const std::string key = "m-" + std::to_string(sequence);
  const json& metadata = Metadata(facts, sequence);
  const std::string turn = JsonValue(metadata, "turn_root", "");
  const std::string work = JsonValue(metadata, "work", "");
  const auto add = [&](json node) {
    node["sequence"] = sequence;
    Keep(node, metadata, {"time"});
    out.push_back(std::move(node));
  };
  // A call, then the files it handed over.
  const auto add_call = [&](const json& record) {
    add(CallNode(record, work));
    if (json files = FilesNode(record, turn); !files.is_null()) {
      add(std::move(files));
    }
  };
  if (entry.kind == "tool_result") {
    std::string id;
    const json* found = ResultRecord(facts, sequence, message, id);
    json record = found ? *found : json::object();
    record["detail_id"] = id;
    // Saved calls finished by definition: a missing receipt is a gap in
    // history, never live work.
    if (!record.contains("state")) record["state"] = "ok";
    add_call(record);
    return;
  }
  const std::string response_id = JsonValue(metadata, "response_id", "");
  const json* tool_calls = JsonArray(message, "tool_calls");
  json node;
  if (entry.kind == "assistant") {
    // The live node streamed under the response id; the saved one is it.
    const std::string id = response_id.empty() ? key : response_id;
    json thinking = ThinkingNode(id, work);
    SetBody(thinking, "text", EntryText(facts, sequence, message, true));
    if (thinking.contains("body")) add(std::move(thinking));
    // A reply that made calls is a step of its work; the one after the
    // last call is its turn's answer.
    node = MakeNode(id, tool_calls && !tool_calls->empty() ? work : turn,
                    "message", {{"role", "assistant"}});
    Keep(node, metadata,
         {"request_id", "route", "served_by", "duration_ms", "ttft_ms",
          "tokens_per_second", "usage", "usage_reported", "http", "files",
          "deliveries", "incoming"});
  } else if (JsonValue(metadata, "origin", "") == "tool") {
    // Files a call read belong to that call.
    const json* sources = JsonArray(metadata, "source_call_ids");
    std::string parent;
    if (!sources || sources->empty() || !(*sources)[0].is_string() ||
        !FindToolFacts(facts, parent, (*sources)[0].get<std::string>())) {
      parent = work;
    }
    node = MakeNode(key, parent, "message", {{"role", "user"}});
    Keep(node, metadata, {"files", "origin"});
  } else {
    // A message of yours starts its turn; one written into a running turn
    // is part of it, and the work after it replies to it.
    const bool starts = turn.empty() || turn == key;
    node = MakeNode(key, starts ? "" : turn, starts ? "turn" : "message");
    if (!starts) node["role"] = "user";
    Keep(node, metadata,
         {"request_id", "files", "origin", "deliveries", "http", "incoming"});
  }
  if (JsonValue(metadata, "origin", "") != "tool") {
    SetBody(node, entry.kind == "assistant" ? "markdown" : "text",
            EntryText(facts, sequence, message));
  }
  if (const json* parts = JsonArray(message, "content")) {
    if (const auto images = std::ranges::count_if(*parts, [](const json& part) {
          return JsonValue(part, "type", "") == "image_url";
        })) {
      node["unavailable_images"] = images;
    }
  }
  // Blank text is never shown: a message with nothing else is no node.
  if (node["kind"] == "turn" || node.contains("body") ||
      node.contains("files") || node.contains("deliveries")) {
    add(std::move(node));
  }
  if (!calls || !tool_calls) return;
  for (size_t index = 0;
       index < std::min(tool_calls->size(), kMaxToolsPerMessage); ++index) {
    const json& call = (*tool_calls)[index];
    const std::string call_id = JsonValue(call, "id", "");
    std::string id = DetailId(response_id, call_id);
    const json* found = FindToolFacts(facts, id, call_id);
    json record = found ? *found : json::object();
    record["detail_id"] = id;
    if (!record.contains("name")) {
      record["name"] = Utf8Trunc(
          JsonValue(JsonValue(call, "function", kNone), "name", ""), 128);
    }
    add_call(record);
  }
}

std::vector<json> LastNodes(const Conversation& conversation) {
  std::vector<json> nodes;
  if (conversation.Empty()) return nodes;
  const size_t last = conversation.Size() - 1;
  EntryNodes(
      conversation, conversation.DisplayIds()[last],
      {&conversation.At(last), MessageKindName(conversation.KindAt(last))},
      false, nodes);
  return nodes;
}

std::string StripAttachedTrailer(const std::string& text) {
  const std::string trailer = std::string(kAttachedList) + "\n";
  const size_t marker = text.rfind(trailer);
  if (marker == std::string::npos) return text;
  size_t pos = marker + trailer.size();
  bool referenced = false;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    const size_t stop = end == std::string::npos ? text.size() : end;
    const std::string_view line(text.data() + pos, stop - pos);
    if (Blank(line)) {
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
