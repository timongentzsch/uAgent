// Copyright 2026 Timon Gentzsch

#include <string>
#include <utility>
#include <vector>

#include "include/agent/conversation.h"
#include "include/agent/node.h"
#include "include/agent/protocol.h"
#include "include/agent/transcript.h"
#include "include/tools/tool.h"
#include "tests/unit/test_support.h"

namespace uagent {
namespace {

// A node in one line: its kind, id and parent, then its state, headline,
// result word and text.
std::string Line(const json& node) {
  const std::string parent = JsonValue(node, "parent", "");
  std::string line = JsonValue(node, "kind", "") + " " +
                     JsonValue(node, "id", "") + " < " +
                     (parent.empty() ? "-" : parent);
  if (const std::string state = JsonValue(node, "state", ""); !state.empty()) {
    line += " [" + state + "]";
  }
  if (const json* headline = JsonObject(node, "headline")) {
    for (const char* part : {"verb", "target"}) {
      if (const std::string text = JsonValue(*headline, part, "");
          !text.empty()) {
        line += " " + text;
      }
    }
  }
  if (const std::string word =
          JsonValue(JsonValue(node, "result", json::object()), "word", "");
      !word.empty()) {
    line += " · " + word;
  }
  if (const json* body = JsonObject(node, "body")) {
    line += " \"" + JsonValue(*body, "text", "") + "\"";
  }
  return line;
}

std::vector<std::string> Lines(const json& nodes) {
  std::vector<std::string> lines;
  for (const json& node : nodes) lines.push_back(Line(node));
  return lines;
}

// A conversation recorded the way an agent records it, and the live view a
// client folds from the same agent's events.
class Recorder {
 public:
  Recorder() {
    conversation_.Reset(json::array({{{"role", "system"}, {"content", "sys"}}}),
                        {MessageKind::kSystem});
  }

  const Conversation& History() const { return conversation_; }
  const json& Live() const { return live_["view"]["blocks"]; }

  void Say(const std::string& text) {
    conversation_.Push({{"role", "user"}, {"content", text}},
                       MessageKind::kUser);
    turn_ = conversation_.LastDisplayId();
    Published({{"turn_root", turn_}});
  }

  // A reply streamed, then saved; `calls` are {id, name} pairs.
  void Reply(
      const std::string& response_id, const std::string& text,
      const std::string& reasoning = "",
      const std::vector<std::pair<std::string, std::string>>& calls = {}) {
    json context = {{"response_id", response_id}, {"turn_root", turn_}};
    Event("response.started", context);
    if (!reasoning.empty()) {
      Event("response.reasoning.delta", Text(context, reasoning));
    }
    if (!text.empty()) Event("response.answer.delta", Text(context, text));
    Event("response.finished", context);
    json message = {{"role", "assistant"}, {"content", text}};
    for (const auto& [id, name] : calls) {
      message["tool_calls"].push_back(
          {{"id", id},
           {"type", "function"},
           {"function", {{"name", name}, {"arguments", "{}"}}}});
    }
    conversation_.Push(std::move(message), MessageKind::kAssistant);
    json facts = {{"turn_root", turn_}, {"response_id", response_id}};
    if (!reasoning.empty()) facts["reasoning"] = reasoning;
    Published(std::move(facts));
  }

  // A call as the tool loop records and announces it, and its result.
  std::string Call(const std::string& response_id, const std::string& call_id,
                   const std::string& name, json view, const std::string& state,
                   const ResultFacts& facts, const std::string& output,
                   const std::string& category = "explore") {
    const std::string id = DetailId(response_id, call_id);
    const json activity = {{"category", category}, {"label", name}};
    conversation_.RecordDisplay(id, {{"activity", activity}, {"view", view}});
    Event("tool.call", {{"detail_id", id},
                        {"name", name},
                        {"activity", activity},
                        {"view", view},
                        {"turn_root", turn_}});
    if (state == "running") return id;
    const json result = ResultJson(facts, state, 5);
    Event("tool.result", {{"detail_id", id},
                          {"state", state},
                          {"facts", result},
                          {"body_kind", "text"},
                          {"result", output},
                          {"activity", activity},
                          {"headline", CallHeadline(view, state, name)}});
    conversation_.RecordDisplay(id, {{"name", name},
                                     {"call_id", call_id},
                                     {"detail_id", id},
                                     {"state", state},
                                     {"result", result},
                                     {"output", output}});
    return id;
  }

  // A step's calls fold into `grouped`, published once the calls are.
  void Group(const std::vector<std::string>& grouped) {
    for (const std::string& call : grouped) {
      conversation_.RecordDisplay(call, {{"group", "g-" + grouped.front()}});
    }
  }

  // What a step's calls added to the turn: their group and the work's counts.
  void Step(const std::vector<std::string>& grouped, const json& counts) {
    if (!grouped.empty()) {
      const std::string id = "g-" + grouped.front();
      const json group = {
          {"verb", "Explored"}, {"calls", grouped.size()}, {"turn", turn_}};
      conversation_.RecordDisplay(id, group);
      Changed(GroupNode(id, group));
    }
    conversation_.RecordDisplay(WorkId(turn_), counts);
    Changed(WorkNode(turn_, counts));
  }

  void Result(const std::string& call_id, const std::string& id) {
    conversation_.Push(
        {{"role", "tool"}, {"tool_call_id", call_id}, {"content", "model"}},
        MessageKind::kToolResult);
    Published({{"turn_root", turn_}, {"detail_id", id}});
  }

  void Footer(double duration_ms, const std::string& outcome) {
    Changed(EntryNode(conversation_.RecordEntry(
        {{"kind", "turn_summary"},
         {"turn_root", turn_},
         {"summary", {{"duration_ms", duration_ms}, {"outcome", outcome}}}})));
  }

 private:
  static json Text(json context, const std::string& text) {
    context["text"] = text;
    return context;
  }
  void Event(const std::string& type, const json& data) {
    ApplySessionEvent(live_, type, data);
  }
  void Changed(const json& node) {
    Event("message.changed", {{"block", node}});
  }
  // The last message's facts recorded, then its nodes published.
  void Published(json facts) {
    conversation_.RecordDisplay(conversation_.LastDisplayId(),
                                std::move(facts));
    for (const json& node : LastNodes(conversation_)) Changed(node);
  }

  Conversation conversation_;
  json live_ = {{"view", {{"blocks", json::array()}}}};
  std::string turn_;
};

const json kRead = {{"verb", {"Reading", "Read"}}, {"target", "a.txt"}};

}  // namespace

// The golden turns: each recorded as an agent records it, read back as a
// saved page, and folded live from the same events into the same nodes.

void TestGoldenPlainAnswer() {
  Recorder turn;
  turn.Say("hi");
  turn.Reply("r-1-1-1", "Hello.");
  turn.Footer(800, "complete");
  const auto page = Lines(ConversationView(turn.History())["blocks"]);
  CHECK((page == std::vector<std::string>{
                     "turn m-2 < - \"hi\"",
                     "message r-1-1-1 < m-2 \"Hello.\"",
                     "footer m-4 < m-2",
                 }));
  CHECK(Lines(turn.Live()) == page);
}

void TestGoldenChainOfCalls() {
  Recorder turn;
  turn.Say("fix it");
  turn.Reply("r-1-1-1", "", "look first",
             {{"c1", "read_path"}, {"c2", "read_path"}, {"c3", "edit_file"}});
  ResultFacts lines;
  lines.items = 1;
  lines.unit = "line";
  const std::string a =
      turn.Call("r-1-1-1", "c1", "read_path", kRead, "ok", lines, "alpha");
  const std::string b =
      turn.Call("r-1-1-1", "c2", "read_path",
                {{"verb", {"Reading", "Read"}}, {"target", "b.txt"}}, "ok",
                lines, "beta");
  ResultFacts change;
  change.added = 1;
  change.removed = 1;
  const std::string c =
      turn.Call("r-1-1-1", "c3", "edit_file",
                {{"verb", {"Editing", "Edited"}}, {"target", "b.txt"}}, "ok",
                change, "edited b.txt", "edit");
  turn.Group({a, b});
  turn.Result("c1", a);
  turn.Result("c2", b);
  turn.Result("c3", c);
  turn.Step({a, b}, {{"items", 3}, {"errors", 0}, {"edited", 1}});
  turn.Reply("r-1-2-1", "Fixed.");
  turn.Footer(124000, "complete");
  const auto page = Lines(ConversationView(turn.History())["blocks"]);
  // The work folds what the turn did, the group the calls that read alike;
  // the answer and the footer close the turn.
  CHECK((page ==
         std::vector<std::string>{
             "turn m-2 < - \"fix it\"",
             "work w-m-2 < m-2 [ok] Worked · 3 steps · edited 1 file · 2m 4s",
             "thinking r-1-1-1:thinking < w-m-2 \"look first\"",
             "group g-" + a + " < w-m-2 [ok] Explored · 2 calls",
             "call " + a + " < g-" + a + " [ok] Read a.txt · 1 line · 5ms " +
                 "\"alpha\"",
             "call " + b + " < g-" + a + " [ok] Read b.txt · 1 line · 5ms " +
                 "\"beta\"",
             "call " + c + " < w-m-2 [ok] Edited b.txt · +1 −1 · 5ms " +
                 "\"edited b.txt\"",
             "message r-1-2-1 < m-2 \"Fixed.\"",
             "footer m-8 < m-2",
         }));
  CHECK(Lines(turn.Live()) == page);
}

void TestGoldenFailedCalls() {
  Recorder turn;
  turn.Say("try");
  turn.Reply("r-1-1-1", "", "",
             {{"f", "read_path"}, {"x", "run"}, {"d", "run"}, {"j", "run"}});
  ResultFacts none;
  ResultFacts denied;
  denied.outcome = "denied";
  ResultFacts rejected;
  rejected.outcome = "rejected";
  rejected.reason = "path is outside the folder";
  const std::string f =
      turn.Call("r-1-1-1", "f", "read_path", kRead, "failed", none, "missing");
  const std::string x = turn.Call(
      "r-1-1-1", "x", "run", {{"verb", {"Running", "Ran"}}, {"target", "make"}},
      "cancelled", none, "", "run");
  const std::string d =
      turn.Call("r-1-1-1", "d", "run",
                {{"verb", {"Running", "Ran"}}, {"target", "rm -rf out"}},
                "failed", denied, "", "run");
  // Refused before it ran: no verb, what it sent.
  const std::string j = turn.Call("r-1-1-1", "j", "{\"path\":\"/etc\"}",
                                  nullptr, "failed", rejected, "", "run");
  for (const auto& [call, id] :
       {std::pair{"f", f}, {"x", x}, {"d", d}, {"j", j}}) {
    turn.Result(call, id);
  }
  turn.Step({}, {{"items", 4}, {"errors", 4}, {"edited", 0}});
  turn.Reply("r-1-2-1", "Nothing worked.");
  turn.Footer(3000, "complete");
  const auto page = Lines(ConversationView(turn.History())["blocks"]);
  CHECK((page ==
         std::vector<std::string>{
             "turn m-2 < - \"try\"",
             "work w-m-2 < m-2 [ok] Worked · 4 steps · 4 errors · 3.0s",
             "call " + f + " < w-m-2 [failed] Read a.txt · failed · 5ms " +
                 "\"missing\"",
             "call " + x + " < w-m-2 [cancelled] Ran make · cancelled · 5ms",
             "call " + d + " < w-m-2 [failed] Ran rm -rf out · denied · 5ms",
             "call " + j + " < w-m-2 [failed] {\"path\":\"/etc\"} · " +
                 "rejected: path is outside the folder · 5ms",
             "message r-1-2-1 < m-2 \"Nothing worked.\"",
             "footer m-9 < m-2",
         }));
  CHECK(Lines(turn.Live()) == page);
}

void TestGoldenBlankReplyAndMcpCall() {
  Recorder turn;
  turn.Say("look it up");
  // A reply that holds only blank text is no node, live or saved.
  turn.Reply("r-1-1-1", " \n", "", {{"m", "mcp__docs__search"}});
  // A tool without a headline of its own reports no facts: it reads as
  // "Called <title>", with its state and how long it took.
  const std::string m =
      turn.Call("r-1-1-1", "m", "mcp__docs__search",
                {{"verb", {"Calling Search", "Called Search"}}}, "ok", {}, "");
  turn.Result("m", m);
  turn.Step({}, {{"items", 1}, {"errors", 0}, {"edited", 0}});
  turn.Reply("r-1-2-1", "\n");
  turn.Footer(1500, "complete");
  const auto page = Lines(ConversationView(turn.History())["blocks"]);
  CHECK((page == std::vector<std::string>{
                     "turn m-2 < - \"look it up\"",
                     "work w-m-2 < m-2 [ok] Worked · 1 step · 1.5s",
                     "call " + m + " < w-m-2 [ok] Called Search · 5ms",
                     "footer m-6 < m-2",
                 }));
  CHECK(Lines(turn.Live()) == page);
}

void TestGoldenRunningTurn() {
  Recorder turn;
  turn.Say("build");
  turn.Reply("r-1-1-1", "Building now.", "", {{"b", "run"}});
  const std::string b = turn.Call(
      "r-1-1-1", "b", "run", {{"verb", {"Running", "Ran"}}, {"target", "make"}},
      "running", {}, "", "run");
  const auto page = Lines(ConversationView(turn.History())["blocks"]);
  // An interim reply is a step of the work, which runs until the footer.
  CHECK((page == std::vector<std::string>{
                     "turn m-2 < - \"build\"",
                     "work w-m-2 < m-2 [running] Working",
                     "message r-1-1-1 < w-m-2 \"Building now.\"",
                     "call " + b + " < w-m-2 [running] Running make",
                 }));
  CHECK(Lines(turn.Live()) == page);
}

void TestGoldenPagesEndOnWholeTurns() {
  Recorder turns;
  const std::string text(3000, 'x');
  for (int index = 0; index < 60; ++index) {
    turns.Say("question " + std::to_string(index));
    turns.Reply("r-" + std::to_string(index) + "-1-1", text);
    turns.Footer(100, "complete");
  }
  const TranscriptView view(turns.History());
  uint64_t before = 0;
  size_t seen = 0;
  for (bool more = true; more;) {
    const json page = view.Page(before);
    const json& nodes = page["blocks"];
    REQUIRE(!nodes.empty());
    // Each page starts on a turn and holds whole turns: three nodes each.
    CHECK(nodes.front()["kind"] == "turn");
    CHECK(nodes.back()["kind"] == "footer");
    CHECK(nodes.size() % 3 == 0);
    CHECK(page["before"] == nodes.front()["sequence"]);
    seen += nodes.size();
    more = page["more"].get<bool>();
    before = page["before"].get<uint64_t>();
  }
  CHECK(seen == 180);
}

}  // namespace uagent
