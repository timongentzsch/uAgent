// Copyright 2026 Timon Gentzsch
// The one reducer of a session's view: the nodes a client holds, patched by
// the session's events and bounded to its newest turns.

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/agent/node.h"
#include "include/agent/transcript.h"
#include "include/core/events.h"
#include "include/core/strings.h"

namespace uagent {
namespace {
std::string_view Field(const json& node, const char* key) {
  const std::string* value = JsonStringRef(node, key);
  return value ? std::string_view(*value) : std::string_view();
}

json::iterator Find(json& nodes, std::string_view id) {
  return std::find_if(nodes.begin(), nodes.end(), [&](const json& node) {
    return Field(node, "id") == id;
  });
}

// Upserts one node without bounding the view; true when it is new or
// different. A new node goes before the first node under it, else in
// sequence position (a late saved node never strands older content after
// newer), else last, as a client holding the view places it too.
bool Upsert(json& nodes, const json& node) {
  const std::string_view id = Field(node, "id");
  auto found = Find(nodes, id);
  if (found == nodes.end()) {
    const uint64_t sequence = JsonValue(node, "sequence", uint64_t{0});
    nodes.insert(
        std::find_if(nodes.begin(), nodes.end(),
                     [&](const json& held) {
                       return Field(held, "parent") == id ||
                              (sequence && JsonValue(held, "sequence",
                                                     uint64_t{0}) > sequence);
                     }),
        node);
    return true;
  }
  // One node per id, whichever side of it arrives: a call and its result
  // fill one node. A saved node has finished streaming, and a shorter preview
  // never replaces streamed text.
  json merged = *found;
  merged.update(node);
  if (node.contains("sequence")) merged.erase("streaming");
  const json* held = JsonObject(*found, "body");
  const json* sent = JsonObject(node, "body");
  if (held && sent && JsonValue(*sent, "truncated", false) &&
      Field(*held, "kind") == Field(*sent, "kind") &&
      Field(*held, "text").size() > Field(*sent, "text").size()) {
    merged["body"] = *held;
  }
  const bool changed = merged != *found;
  *found = std::move(merged);
  return changed;
}

// Drops the oldest nodes past the view's bounds, in one walk of the view,
// and then up to the next turn, so the view starts where one does; returns
// how many went.
size_t BoundView(json& view) {
  json& nodes = view["nodes"];
  size_t bytes = JsonEstimatedBytes(nodes);
  size_t dropped = 0;
  while (dropped < nodes.size() &&
         (nodes.size() - dropped > kViewNodes || bytes > kViewBytes)) {
    bytes -= std::min(bytes, JsonEstimatedBytes(nodes[dropped]));
    ++dropped;
  }
  if (dropped) {
    const auto turn = std::find_if(
        nodes.begin() + static_cast<std::ptrdiff_t>(dropped), nodes.end(),
        [](const json& node) { return Field(node, "kind") == "turn"; });
    if (turn != nodes.end()) {
      dropped = static_cast<size_t>(turn - nodes.begin());
    }
    nodes.erase(nodes.begin(),
                nodes.begin() + static_cast<std::ptrdiff_t>(dropped));
    view["more"] = true;
  }
  const auto first =
      std::find_if(nodes.begin(), nodes.end(),
                   [](const json& node) { return node.contains("sequence"); });
  if (first != nodes.end()) {
    view["before"] = JsonValue(*first, "sequence", uint64_t{0});
  }
  return dropped;
}

// The turn a held node is under, through its group, work and the message
// the work replies to; "" when it is under none the view holds.
std::string TurnOf(json& nodes, std::string_view id) {
  for (int up = 0; up < 5 && !id.empty(); ++up) {
    const auto found = Find(nodes, id);
    if (found == nodes.end()) break;
    if (Field(*found, "kind") == "turn") return std::string(id);
    id = Field(*found, "parent");
  }
  return {};
}

json& ViewNodes(json& view) {
  if (!view.is_object()) view = json::object();
  json& nodes = view["nodes"];
  if (!nodes.is_array()) nodes = json::array();
  return nodes;
}

// The {kind:"node"} frames of the nodes in `ids` the view still holds.
void Patches(const json& nodes, const std::vector<std::string>& ids,
             json& patches) {
  for (const std::string& id : ids) {
    const auto found =
        std::find_if(nodes.begin(), nodes.end(),
                     [&](const json& node) { return Field(node, "id") == id; });
    if (found != nodes.end()) {
      patches.push_back({{"kind", "node"}, {"node", *found}});
    }
  }
}
}  // namespace

size_t MergeNodes(json& view, const json& nodes, json* patches) {
  json& held = ViewNodes(view);
  std::vector<std::string> changed;
  for (const json& node : nodes) {
    if (Upsert(held, node)) changed.emplace_back(Field(node, "id"));
  }
  const size_t dropped = BoundView(view);
  if (patches) {
    *patches = json::array();
    Patches(view["nodes"], changed, *patches);
  }
  return dropped;
}

bool ApplySessionEvent(json& state, const std::string& type, const json& data,
                       json* patches) {
  if (type == AppType(EventId::kUsageUpdated)) {
    state["usage"] = JsonValue(data, "usage", json(nullptr));
    if (data.contains("context_tokens")) {
      state["context_tokens"] = data["context_tokens"];
    }
    if (data.contains("statistics")) state["statistics"] = data["statistics"];
    return true;
  }
  if (type == AppType(EventId::kActivitiesChanged)) {
    state["activities"] = JsonValue(data, "activities", json(nullptr));
    return true;
  }
  if (type == AppType(EventId::kHttpExchange)) {
    state["http"] = json::array({data});
    return true;
  }
  if (type == AppType(EventId::kConfigChanged)) {
    const json* permissions = JsonObject(data, "permissions");
    if (!permissions) return false;
    state["permissions"] = *permissions;
    state["yolo"] = JsonValue(*permissions, "effective", "") == "yolo";
    return true;
  }
  const bool answer = type == AppType(EventId::kAnswerDelta);
  if (!answer && type != AppType(EventId::kReasoningDelta) &&
      type != AppType(EventId::kResponseFinished) &&
      type != AppType(EventId::kToolCall) &&
      type != AppType(EventId::kToolResult) &&
      type != AppType(EventId::kMessageChanged)) {
    return false;
  }
  json& nodes = ViewNodes(state["view"]);
  // Whole nodes go out once the event is applied and the view bounded, in
  // the order they changed: a step's work after the step it goes before.
  std::vector<std::string> changed;
  const auto upsert = [&](const json& node) {
    if (Upsert(nodes, node)) changed.emplace_back(Field(node, "id"));
    const std::string_view parent = Field(node, "parent");
    if (parent.starts_with("w-") && Find(nodes, parent) == nodes.end()) {
      Upsert(nodes, WorkNode(std::string(parent.substr(2)), json::object()));
      changed.emplace_back(parent);
    }
  };
  const std::string response_id = JsonValue(data, "response_id", "");
  if (answer || type == AppType(EventId::kReasoningDelta)) {
    // Deltas without a response id belong to no reply; the whole node still
    // arrives with message.changed.
    if (response_id.empty()) return true;
    // A reply, and its thinking as a node of its own, start with their first
    // delta.
    const std::string id = answer ? response_id : ThinkingId(response_id);
    auto found = Find(nodes, id);
    if (found == nodes.end()) {
      nodes.push_back(
          answer ? MakeNode(id, JsonValue(data, "turn_root", ""), "message",
                            {{"role", "assistant"}, {"streaming", true}})
                 : ThinkingNode(response_id, JsonValue(data, "work", ""),
                                {{"streaming", true}}));
      found = nodes.end() - 1;
    }
    if (!JsonValue(*found, "streaming", false)) return true;
    json& body = (*found)["body"];
    if (!body.is_object() || !body["text"].is_string()) {
      body = {{"kind", answer ? "markdown" : "text"}, {"text", ""}};
    }
    std::string& text = body["text"].get_ref<std::string&>();
    const bool shown = !Blank(text);
    const std::string* sent = JsonStringRef(data, "text");
    const std::string_view delta = sent ? *sent : std::string_view();
    const bool reset = !answer && JsonValue(data, "reset", false);
    std::string_view added = delta;
    if (reset) {
      text = delta;
    } else {
      if (data.contains("offset")) {
        const size_t offset = JsonValue(data, "offset", size_t{0});
        // A checkpoint may already hold this delta. Only the still-missing
        // suffix is appended, when its overlapping bytes agree; a gap or a
        // conflict waits for the whole node.
        if (offset > text.size()) return true;
        const size_t overlap = std::min(delta.size(), text.size() - offset);
        if (text.compare(offset, overlap, delta.substr(0, overlap)) != 0) {
          return true;
        }
        added = delta.substr(overlap);
      }
      if (added.empty()) return true;
      // Appended in place: a long answer is never copied per delta.
      text += added;
    }
    // Blank text is never shown, so a client first gets the whole node.
    if (Blank(text)) return true;
    if (shown && !reset) {
      if (patches) {
        patches->push_back(
            {{"kind", "node"}, {"id", id}, {"append", {{"text", added}}}});
      }
      return true;
    }
    changed.emplace_back(id);
    upsert(json(*found));
  } else if (type == AppType(EventId::kResponseFinished)) {
    for (const std::string& id : {response_id, ThinkingId(response_id)}) {
      auto found = Find(nodes, id);
      if (found == nodes.end() || !JsonValue(*found, "streaming", false)) {
        continue;
      }
      // A reply that showed nothing leaves nothing; no client holds it.
      if (const json* body = JsonObject(*found, "body");
          !body || Blank(Field(*body, "text"))) {
        nodes.erase(found);
        continue;
      }
      (*found)["streaming"] = false;
      if (patches) {
        patches->push_back(
            {{"kind", "node"}, {"id", id}, {"set", {{"streaming", false}}}});
      }
    }
  } else if (type == AppType(EventId::kToolCall)) {
    if (JsonValue(data, "detail_id", "").empty()) return true;
    upsert(CallNode(data, JsonValue(data, "work", "")));
  } else if (type == AppType(EventId::kToolResult)) {
    if (JsonValue(data, "detail_id", "").empty()) return true;
    // The output is what a person reads; the model's hints never reach it.
    json record = {{"detail_id", data["detail_id"]},
                   {"state", JsonValue(data, "state", "ok")},
                   {"result", JsonValue(data, "facts", json::object())},
                   {"body_kind", JsonValue(data, "body_kind", "text")},
                   {"output", JsonValue(data, "result", "")}};
    for (const char* key : {"headline", "activity", "parts"}) {
      if (data.contains(key)) record[key] = data[key];
    }
    json node = CallNode(record, "");
    // The call's own event placed it; its result leaves it there.
    node.erase("parent");
    upsert(node);
    if (json files =
            FilesNode(record, TurnOf(nodes, Field(record, "detail_id")));
        !files.is_null()) {
      upsert(files);
    }
  } else if (const json* node = JsonObject(data, "node")) {
    // Work is made by the first node under it: alone, it only changes.
    if (Field(*node, "kind") != "work" ||
        Find(nodes, Field(*node, "id")) != nodes.end()) {
      upsert(*node);
    }
  }
  BoundView(state["view"]);
  if (patches) Patches(state["view"]["nodes"], changed, *patches);
  return true;
}

}  // namespace uagent
