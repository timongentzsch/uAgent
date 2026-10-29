// Copyright 2026 Timon Gentzsch

#include <string>
#include <utility>
#include <vector>

#include "include/agent/conversation.h"
#include "include/agent/node.h"
#include "include/agent/protocol.h"
#include "include/agent/transcript.h"
#include "include/core/detail.h"
#include "include/tools/tool.h"
#include "include/ui/presentation.h"
#include "tests/unit/terminal_test_support.h"
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

// Terminal output without its control sequences.
std::string Plain(std::string_view drawn) {
  std::string plain;
  for (size_t at = 0; at < drawn.size();) {
    const size_t span = drawn[at] == '\r' ? 1 : CsiSpan(drawn.substr(at));
    if (span) {
      at += span;
    } else {
      plain += drawn[at++];
    }
  }
  return plain;
}

// What a terminal draws of `nodes` at the three levels, depth 0, 1 and all.
std::vector<std::string> Drawings(const json& nodes) {
  std::vector<std::string> drawings;
  for (const char* level : {"minimal", "default", "full"}) {
    TerminalPresenter presenter;
    presenter.SetDetail(DetailFor(level));
    drawings.push_back(
        Plain(CaptureStdout([&] { presenter.Draw({{"blocks", nodes}}); })));
  }
  return drawings;
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
  // What a terminal at `level` draws as the events arrive: the replies as
  // they stream, and the tree's nodes as they settle.
  void Watch(std::string_view level) { watcher_.SetDetail(DetailFor(level)); }
  std::string Watched() const { return Plain(watched_); }

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
                   const std::string& category = "explore",
                   const json& parts = nullptr) {
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
                          {"parts", parts},
                          {"activity", activity},
                          {"headline", CallHeadline(view, state, name)}});
    conversation_.RecordDisplay(id, {{"name", name},
                                     {"call_id", call_id},
                                     {"detail_id", id},
                                     {"state", state},
                                     {"result", result},
                                     {"output", output},
                                     {"parts", parts}});
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
    watched_ += CaptureStdout([&] {
      watcher_.Consume(AppEvent{0, "", type, data, false});
      // As a terminal holds its tree: calls and nodes, not their results.
      if (type == "tool.call" || type == "message.changed") {
        ApplySessionEvent(terminal_, type, data);
      }
      if (const json* node = JsonObject(data, "block")) {
        watcher_.Update(terminal_["view"], *node);
      }
    });
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
  json terminal_ = {{"view", {{"blocks", json::array()}}}};
  TerminalPresenter watcher_;
  std::string watched_;
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
  // The terminal draws the same at every depth: nothing is folded.
  const std::string plain = "> hi\nuagent\nHello.\n0 in · 0 out · 800ms\n";
  CHECK((Drawings(ConversationView(turn.History())["blocks"]) ==
         std::vector<std::string>{plain, plain, plain}));
  CHECK(turn.Watched() == plain);
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
             "work w-m-2 < m-2 [ok] Worked 3 steps · edited 1 file · 2m 4s",
             "thinking r-1-1-1:thinking < w-m-2 Thinking \"look first\"",
             "group g-" + a + " < w-m-2 [ok] Explored 2 calls",
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
  // The terminal opens the work at depth 1 and everything at depth all; an
  // open node adds its body's tail, and closed thinking says nothing.
  CHECK((Drawings(ConversationView(turn.History())["blocks"]) ==
         std::vector<std::string>{
             "> fix it\n"
             "  · Worked 3 steps · edited 1 file · 2m 4s\n"
             "uagent\nFixed.\n0 in · 0 out · 2m 4s\n",
             "> fix it\n"
             "  · Worked 3 steps · edited 1 file · 2m 4s\n"
             "    · Explored 2 calls\n"
             "    · Edited b.txt · +1 −1 · 5ms\n"
             "uagent\nFixed.\n0 in · 0 out · 2m 4s\n",
             "> fix it\n"
             "  · Worked 3 steps · edited 1 file · 2m 4s\n"
             "    · Thinking\n"
             "      │ look first\n"
             "    · Explored 2 calls\n"
             "      · Read a.txt · 1 line · 5ms\n"
             "        │ alpha\n"
             "      · Read b.txt · 1 line · 5ms\n"
             "        │ beta\n"
             "    · Edited b.txt · +1 −1 · 5ms\n"
             "      │ edited b.txt\n"
             "uagent\nFixed.\n0 in · 0 out · 2m 4s\n",
         }));
  // Live, a step is drawn once it ends, in the tree's order, and the work's
  // line once the turn does: after the answer, which streamed.
  CHECK(turn.Watched() ==
        "> fix it\n"
        "    · Explored 2 calls\n"
        "    · Edited b.txt · +1 −1 · 5ms\n"
        "uagent\nFixed.\n"
        "  · Worked 3 steps · edited 1 file · 2m 4s\n"
        "0 in · 0 out · 2m 4s\n");
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
  // Refused before it ran: no verb, the call it would have made.
  const std::string j = turn.Call("r-1-1-1", "j", "Read /etc", nullptr,
                                  "failed", rejected, "", "run");
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
             "work w-m-2 < m-2 [ok] Worked 4 steps · 4 errors · 3.0s",
             "call " + f + " < w-m-2 [failed] Read a.txt · failed · 5ms " +
                 "\"missing\"",
             "call " + x + " < w-m-2 [cancelled] Ran make · cancelled · 5ms",
             "call " + d + " < w-m-2 [failed] Ran rm -rf out · denied · 5ms",
             "call " + j + " < w-m-2 [failed] Read /etc · " +
                 "rejected: path is outside the folder · 5ms",
             "message r-1-2-1 < m-2 \"Nothing worked.\"",
             "footer m-9 < m-2",
         }));
  CHECK(Lines(turn.Live()) == page);
  // Each call's state marks its row, and the host's words say why.
  const std::string calls =
      "    ✗ Read a.txt · failed · 5ms\n"
      "    · Ran make · cancelled · 5ms\n"
      "    ✗ Ran rm -rf out · denied · 5ms\n"
      "    ✗ Read /etc · rejected: path is outside the folder · 5ms\n";
  CHECK((Drawings(ConversationView(turn.History())["blocks"]) ==
         std::vector<std::string>{
             "> try\n  · Worked 4 steps · 4 errors · 3.0s\n"
             "uagent\nNothing worked.\n0 in · 0 out · 3.0s\n",
             "> try\n  · Worked 4 steps · 4 errors · 3.0s\n" + calls +
                 "uagent\nNothing worked.\n0 in · 0 out · 3.0s\n",
             "> try\n  · Worked 4 steps · 4 errors · 3.0s\n"
             "    ✗ Read a.txt · failed · 5ms\n"
             "      │ missing\n" +
                 calls.substr(calls.find("    · Ran")) +
                 "uagent\nNothing worked.\n0 in · 0 out · 3.0s\n",
         }));
  CHECK(turn.Watched() == "> try\n" + calls +
                              "uagent\nNothing worked.\n"
                              "  · Worked 4 steps · 4 errors · 3.0s\n"
                              "0 in · 0 out · 3.0s\n");
}

void TestGoldenFileHandedOver() {
  Recorder turn;
  turn.Say("make a report");
  turn.Reply("r-1-1-1", "", "", {{"s", "share"}});
  const json file = {{"kind", "file"},
                     {"id", "0123456789abcdef"},
                     {"name", "report.html"},
                     {"mime", "text/html"},
                     {"bytes", 2048}};
  const json link = {
      {"kind", "link"}, {"to", "activity"}, {"id", 7}, {"label", "Open"}};
  const std::string s =
      turn.Call("r-1-1-1", "s", "share",
                {{"verb", {"Sharing", "Shared"}}, {"target", "report.html"}},
                "ok", {}, "", "share", json::array({file, link}));
  turn.Result("s", s);
  turn.Step({}, {{"items", 1}, {"errors", 0}, {"edited", 0}});
  turn.Reply("r-1-2-1", "Here it is.");
  turn.Footer(2000, "complete");
  const json view = ConversationView(turn.History());
  const auto page = Lines(view["blocks"]);
  // The file is the turn's, so it shows at every depth; the call keeps its
  // other parts.
  CHECK((page == std::vector<std::string>{
                     "turn m-2 < - \"make a report\"",
                     "work w-m-2 < m-2 [ok] Worked 1 step · 2.0s",
                     "call " + s + " < w-m-2 [ok] Shared report.html · 5ms",
                     "message " + s + ":files < m-2",
                     "message r-1-2-1 < m-2 \"Here it is.\"",
                     "footer m-6 < m-2",
                 }));
  CHECK(Lines(turn.Live()) == page);
  CHECK(view["blocks"][2]["parts"] == json::array({link}));
  CHECK(view["blocks"][3]["parts"] == json::array({file}));
  // The terminal lists the file under the turn at every depth; an open call
  // adds the parts it kept.
  const std::string work = "> make a report\n  · Worked 1 step · 2.0s\n";
  const std::string call = "    · Shared report.html · 5ms\n";
  const std::string rest =
      "  ↳ report.html · 2 kB\nuagent\nHere it is.\n0 in · 0 out · 2.0s\n";
  CHECK((Drawings(view["blocks"]) ==
         std::vector<std::string>{work + rest, work + call + rest,
                                  work + call + "      ↳ Open · 7\n" + rest}));
  CHECK(turn.Watched() ==
        "> make a report\n" + call +
            "  ↳ report.html · 2 kB\nuagent\nHere it is.\n"
            "  · Worked 1 step · 2.0s\n0 in · 0 out · 2.0s\n");
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
                     "work w-m-2 < m-2 [ok] Worked 1 step · 1.5s",
                     "call " + m + " < w-m-2 [ok] Called Search · 5ms",
                     "footer m-6 < m-2",
                 }));
  CHECK(Lines(turn.Live()) == page);
  // Blank replies draw nothing, live or saved; the call reads as its title.
  const std::string work = "  · Worked 1 step · 1.5s\n";
  const std::string call = "    · Called Search · 5ms\n";
  const std::string footer = "0 in · 0 out · 1.5s\n";
  CHECK((Drawings(ConversationView(turn.History())["blocks"]) ==
         std::vector<std::string>{"> look it up\n" + work + footer,
                                  "> look it up\n" + work + call + footer,
                                  "> look it up\n" + work + call + footer}));
  CHECK(turn.Watched() == "> look it up\n" + call + work + footer);
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
  // A whole drawing shows running work as it stands, the interim reply
  // indented under it; live, nothing has settled but the reply that
  // streamed.
  const std::string open =
      "> build\n  → Working\n    Building now.\n    → Running make\n";
  CHECK((Drawings(ConversationView(turn.History())["blocks"]) ==
         std::vector<std::string>{"> build\n  → Working\n", open, open}));
  CHECK(turn.Watched() == "> build\nuagent\nBuilding now.");
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

// A compaction says what it kept and how long it took.
void TestCompactionNoticeSaysWhatWent() {
  const json node = EntryNode({{"kind", "compaction"},
                               {"id", "a-1"},
                               {"compaction",
                                {{"messages_before", 120},
                                 {"messages_after", 40},
                                 {"duration_ms", 1200}}}});
  CHECK(node["kind"] == "notice");
  CHECK(node["result"]["word"] == "120 → 40 messages · 1.2s");
}

}  // namespace uagent
