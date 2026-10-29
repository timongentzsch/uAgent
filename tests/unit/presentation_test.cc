// Copyright 2026 Timon Gentzsch

#include "include/ui/presentation.h"

#include <unistd.h>

#include <chrono>
#include <clocale>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "include/agent/child_agent.h"
#include "include/agent/node.h"
#include "include/agent/transcript.h"
#include "include/cli/options.h"
#include "include/core/defaults.h"
#include "include/core/style.h"
#include "include/core/term.h"
#include "include/core/turn_status.h"
#include "include/tools/registry.h"
#include "include/tools/tool.h"
#include "include/ui/display.h"
#include "tests/unit/terminal_test_support.h"

namespace uagent {
namespace {

TurnStatusView Working(std::chrono::milliseconds elapsed) {
  TurnStatusView view;
  view.elapsed = elapsed;
  view.context_used = 12000;
  view.context_window = 1300000;
  return view;
}

// Render the idle row at a known width. TerminalColumns() probes TIOCGWINSZ on
// both standard descriptors before it falls back to COLUMNS, so both have to
// be something that is not a terminal -- otherwise this passes under ctest,
// where they are pipes, and silently measures the developer's window when the
// binary is run by hand.
class FixedWidth {
 public:
  explicit FixedWidth(int columns) {
    const char* prior = getenv("COLUMNS");
    had_columns_ = prior != nullptr;
    if (prior) saved_columns_ = prior;
    setenv("COLUMNS", std::to_string(columns).c_str(), 1);
    empty_ = tmpfile();
    if (empty_) {
      saved_out_ = dup(STDOUT_FILENO);
      saved_in_ = dup(STDIN_FILENO);
      dup2(fileno(empty_), STDOUT_FILENO);
      dup2(fileno(empty_), STDIN_FILENO);
    }
  }
  ~FixedWidth() {
    if (saved_out_ >= 0) {
      dup2(saved_out_, STDOUT_FILENO);
      close(saved_out_);
    }
    if (saved_in_ >= 0) {
      dup2(saved_in_, STDIN_FILENO);
      close(saved_in_);
    }
    if (empty_) fclose(empty_);
    if (had_columns_) {
      setenv("COLUMNS", saved_columns_.c_str(), 1);
    } else {
      unsetenv("COLUMNS");
    }
  }
  FixedWidth(const FixedWidth&) = delete;
  FixedWidth& operator=(const FixedWidth&) = delete;

 private:
  bool had_columns_ = false;
  std::string saved_columns_;
  FILE* empty_ = nullptr;
  int saved_out_ = -1;
  int saved_in_ = -1;
};

// What was recorded of a call in `state`, its result worded `word`, or by
// its state when it did not succeed.
json Record(const std::string& id, const std::string& name,
            const std::string& state, const std::string& text,
            std::string word = "") {
  if (word.empty() && state != "ok") word = state;
  return {{"detail_id", "t-" + id},
          {"name", name},
          {"state", state},
          {"result", {{"word", word}}},
          {"output", text}};
}

// A turn whose work holds `calls`: what a terminal draws of it at `level`.
std::string Drawn(const std::vector<json>& calls,
                  std::string_view level = "default") {
  json nodes =
      json::array({{{"id", "m-1"}, {"parent", nullptr}, {"kind", "turn"}},
                   WorkNode("m-1", json::object())});
  for (const json& call : calls) nodes.push_back(call);
  return CaptureStdout([&] {
    TerminalPresenter presenter;
    presenter.SetDetail(DetailFor(level));
    presenter.Draw({{"nodes", nodes}});
  });
}

}  // namespace

// A node's row is its headline and the host's words, marked and coloured by
// its state; opened, it adds its body's last lines. The terminal reads
// nothing else of a call.
void TestTreeRows() {
  const std::string failure_report = ChildAgentFailureReport(
      "provider/child @ child.example", ChildAgentFailureStage::kExecution,
      "connection error: Couldn't connect to server\n"
      "api_key=child-secret-must-not-enter-row");
  json record = Record("c1", "subagent", "failed", failure_report);
  record["view"] = {{"verb", {"Delegating", "Delegated"}}, {"target", "x"}};
  // A failed delegation's row says so; what the child printed stays in its
  // body, which only an open row shows.
  std::string drawn = Drawn({CallNode(record, "m-1")});
  CHECK(drawn.find("    ✗ Delegated x · failed\n") != std::string::npos);
  CHECK(drawn.find("child-secret-must-not-enter-row") == std::string::npos);
  CHECK(Drawn({CallNode(record, "m-1")}, "full").find("      │ ") !=
        std::string::npos);

  // Closed, a call is one line, whatever its output says; only the recorded
  // state marks a failure.
  json command = Record("c2", "run", "ok", "error: printed by command");
  command["view"] = {{"verb", {"Running", "Ran"}}, {"target", "make\nmore"}};
  drawn = Drawn({CallNode(command, "m-1")});
  CHECK(drawn.find("    · Ran make\n") != std::string::npos);
  CHECK(drawn.find("printed by command") == std::string::npos);
  CHECK(drawn.find("more") == std::string::npos);
  // Opened, a long output shows its end, where a command says how it went,
  // without the command's own colouring.
  command["body_kind"] = "tail";
  command["truncated"] = true;
  command["tail"] = "98\n\x1b[31m99\x1b[0m\n100\n101\n";
  drawn = Drawn({CallNode(command, "m-1")}, "full");
  CHECK(drawn.find("      │ 99\n      │ 100\n      │ 101\n") !=
        std::string::npos);
  CHECK(drawn.find("98") == std::string::npos);
  CHECK(drawn.find("\\x1b") == std::string::npos);

  // A label is stored text: shown, never obeyed.
  CHECK(Drawn({GroupNode("g-t-c1", {{"verb", "Explored\x1b[2J"},
                                    {"calls", 2},
                                    {"turn", "m-1"}})})
            .find("Explored\\x1b[2J 2 calls") != std::string::npos);

  // A terminal whose locale cannot decode UTF-8 gets the same rows in ASCII
  // rather than mojibake. Model/tool content must remain byte-preserved.
  json content = Record("c4", "run", "failed", "model text: µ · ← …");
  content["view"] = {{"verb", {"Running", "Ran"}}, {"target", "µ"}};
  bool prior_unicode = g_unicode;
  g_unicode = false;
  drawn = Drawn({CallNode(content, "m-1")}, "full");
  CHECK(drawn.find("    x Ran µ - failed\n") != std::string::npos);
  CHECK(drawn.find("      | model text: µ · ← …") != std::string::npos);
  CHECK(StatusBarLine("thinking · 2s").find("thinking - 2s") !=
        std::string::npos);

  // Plain mode speaks each row's kind instead of drawing its glyph.
  g_plain = true;
  CHECK(Drawn({CallNode(content, "m-1")}).find("failed: Ran µ") !=
        std::string::npos);
  CHECK(Note(Tone::kError, "gone") == "error: gone\n");
  CHECK(Note(Tone::kNeutral, "compacted") == "compacted\n");
  g_plain = false;
  g_unicode = prior_unicode;

  // Only an explicitly non-UTF-8 locale downgrades; an unset one is no
  // evidence that the terminal is incapable.
  // Copied: setenv may free the string getenv returned.
  const char* lc_all = getenv("LC_ALL");
  const std::optional<std::string> prior_lc_all =
      lc_all ? std::optional<std::string>(lc_all) : std::nullopt;
  setenv("LC_ALL", "C", 1);
  CHECK(!ResolveUnicodeEnabled());
  setenv("LC_ALL", "en_US.UTF-8", 1);
  CHECK(ResolveUnicodeEnabled());
  if (prior_lc_all) {
    setenv("LC_ALL", prior_lc_all->c_str(), 1);
  } else {
    unsetenv("LC_ALL");
  }

  // A process started with no locale still measures conversation text.
  std::setlocale(LC_CTYPE, "C");
  CHECK(EnsureUtf8Ctype());
  CHECK(DisplayWidth("\xe4\xb8\xad") == 2);
}

// Live, a node is drawn once its step has ended, as a resumed one is drawn;
// a whole drawing shows a running node as it stands, and its end once more.
void TestTreeDrawsLiveAsResumed() {
  json record = Record("c1", "read_path", "running", "");
  record["view"] = {{"verb", {"Reading", "Read"}}, {"target", "a.txt"}};
  record["result"] = json::object();
  const json work = WorkNode("m-1", json::object());
  json view = {
      {"nodes",
       json::array({{{"id", "m-1"}, {"parent", nullptr}, {"kind", "turn"}},
                    work,
                    CallNode(record, "m-1")})}};
  TerminalPresenter presenter;
  std::string drawn = CaptureStdout([&] { presenter.Draw(view); });
  CHECK(drawn.find("  → Working\n    → Reading a.txt\n") != std::string::npos);
  // Nothing settled: nothing more.
  CHECK(CaptureStdout([&] { presenter.Update(view, work); }).empty());
  record["state"] = "ok";
  record["output"] = "alpha";
  view["nodes"][2] = CallNode(record, "m-1");
  // A call that settled waits for its step to end, then is drawn once.
  CHECK(
      CaptureStdout([&] { presenter.Update(view, view["nodes"][2]); }).empty());
  const std::string settled = CaptureStdout([&] {
    presenter.Update(view, work);
    presenter.Update(view, work);
  });
  CHECK(settled == "    · Read a.txt\n");
  CHECK(Drawn({CallNode(record, "m-1")}).ends_with(settled));
  // What a terminal echoed as it was typed is not drawn again.
  const json echoed = {{"id", "m-2"},
                       {"parent", nullptr},
                       {"kind", "turn"},
                       {"body", {{"kind", "text"}, {"text", "next"}}}};
  view["nodes"].push_back(echoed);
  presenter.Drawn("m-2");
  CHECK(CaptureStdout([&] { presenter.Update(view, echoed); }).empty());

  // A reply holding no text prints no bare mark line.
  CHECK(CaptureStdout([] {
          TerminalPresenter replies;
          replies.Draw({{"nodes", json::array({{{"id", "r-1"},
                                                {"parent", nullptr},
                                                {"kind", "message"},
                                                {"role", "assistant"}}})}});
        }).empty());
}

void TestToolViews() {
  // The generic view: short scalars are fields, long text is code, nested
  // values are indented JSON, and display labels are never repeated.
  json view = ToolView(nullptr, {{"path", "a.txt"},
                                 {"limit", 20},
                                 {"intent", "read"},
                                 {"body", "one\ntwo"},
                                 {"edits", json::array({{{"old", "x"}}})}});
  // A call without a headline of its own is "Called <tool>", and an
  // argument too long for a line never stands in for its target.
  CHECK(view["verb"] == json::array({"Calling tool", "Called tool"}));
  CHECK(view["target"] == "a.txt");
  CHECK(!view.contains("output"));
  const json& input = view["input"];
  CHECK(input[0]["kind"] == "fields");
  CHECK(input[0]["rows"].size() == 2);
  CHECK(input.dump().find("intent") == std::string::npos);
  CHECK(input[1]["kind"] == "code" && input[1]["label"] == "body" &&
        input[1]["text"] == "one\ntwo");
  CHECK(input[2]["language"] == "json");
  // A shell line is shown verbatim, never as escaped JSON.
  Tool run;
  run.present = [](const json& a) {
    return json::array({CommandPart(JsonValue(a, "command", ""))});
  };
  const std::string command = R"(psql -c "SELECT 'x'")";
  CHECK(ToolView(&run, {{"command", command}})["input"][0]["text"] == command);
  // Unparseable arguments still render, as the raw text.
  CHECK(ToolView(nullptr, json("{broken"))["input"][0]["text"] == "{broken");
  // A plan reads as its progress; its result shows the list.
  ActivitySupervisor supervisor;
  const std::vector<Tool> tools = BuiltinTools(supervisor);
  const json plan = ToolView(FindTool(tools, "plan"),
                             {{"items",
                               {{{"text", "a"}, {"status", "done"}},
                                {{"text", "b"}, {"status", "pending"}}}}});
  CHECK(plan["verb"] == json::array({"Updating plan", "Updated plan"}));
  CHECK(plan["target"] == "1/2 done" && plan["input"].empty());
}

// The model reads the output, then the hints; a person reads the output.
void TestModelHints() {
  ToolResult result = ToolSuccess("done");
  AddHint(result, "\n[collaborator agent-1; resume]");
  AddHint(result, "[clamped x]");
  CHECK(result.hint == "[collaborator agent-1; resume]\n[clamped x]");
  CHECK(ModelText(result) ==
        "done\n[collaborator agent-1; resume]\n[clamped x]");
  // A hint alone does not open on an empty line.
  ToolResult started = ToolSuccess("");
  AddHint(started, "[started] subagent agent-1");
  CHECK(ModelText(started) == "[started] subagent agent-1");
  // A captured log's locator follows the hints, whatever cut the output.
  result.artifact = ToolArtifact{"/tmp/log.txt", 99};
  CHECK(ModelText(result).ends_with(ArtifactHint(*result.artifact)));
  CHECK(ArtifactHint(*result.artifact).starts_with(kCapturedLogMarker));
}

// One function words a result from its facts, in a fixed order of what
// matters most, then its duration.
void TestResultWord() {
  auto word = [](const ResultFacts& facts, std::string_view state = "ok",
                 double ms = -1) { return ResultWord(facts, state, ms); };
  // A tool that reports no facts reads as its duration alone, or its state.
  CHECK((word({}, "ok", 1400) == "1.4s"));
  CHECK((word({}) == ""));
  CHECK((word({}, "failed", 20) == "failed · 20ms"));
  CHECK((word({}, "cancelled") == "cancelled"));
  CHECK((word({.outcome = "timed_out"}, "failed") == "timed out"));
  CHECK((word({.outcome = "denied"}, "failed") == "denied"));
  CHECK((word({.outcome = "rejected", .reason = "path is outside"}, "failed") ==
         "rejected: path is outside"));
  CHECK((word({.exit = ResultFacts::Exit{.signal = 9}}, "failed") == "killed"));
  CHECK((word({.exit = ResultFacts::Exit{.code = 2}}, "failed", 1400) ==
         "exit 2 · 1.4s"));
  // A clean exit says nothing beyond the time it took.
  CHECK((word({.exit = ResultFacts::Exit{}}, "ok", 1400) == "1.4s"));
  CHECK((word({.added = 3, .removed = 1}) == "+3 −1"));
  CHECK((word({.added = 0, .removed = 0, .no_change = true}) == "no change"));
  CHECK((word({.items = 1, .unit = "line"}) == "1 line"));
  CHECK((word({.items = 12, .unit = "entry"}) == "12 entries"));
  CHECK((word({.items = 2, .unit = "match"}) == "2 matches"));
  // The first that applies wins: a refusal over its exit, an exit over a
  // count, a count over the bare state.
  CHECK((word({.exit = ResultFacts::Exit{.code = 1}, .outcome = "denied"},
              "failed") == "denied"));
  CHECK(
      (word({.exit = ResultFacts::Exit{.code = 1}, .items = 3, .unit = "line"},
            "failed") == "exit 1"));
  CHECK((word({.items = 0, .unit = "line"}, "failed") == "0 lines"));
  // The row carries the facts it was worded from.
  const json result = ResultJson(
      {.exit = ResultFacts::Exit{.code = 2}, .items = 4, .unit = "line"},
      "failed", 10);
  CHECK(result["word"] == "exit 2 · 10ms");
  CHECK((result["exit"] == json{{"code", 2}}));
  CHECK(result["items"] == 4 && result["unit"] == "line");
  CHECK(!result.contains("added") && !result.contains("outcome"));
  CHECK(ResultState(CompletionStatus::kTimedOut) == std::string("failed"));
  CHECK(ResultState(CompletionStatus::kCancelled) == std::string("cancelled"));
}

void TestDiffLineColoring() {
  const std::string diff =
      "target: /tmp/config\n\n--- /tmp/config\n+++ /tmp/config\n"
      "  KEEP=1\n- OLD=1\n+ NEW=1\n";
  bool prior = g_color;
  g_color = true;
  std::string colored = ColorizeDiffLines(diff);
  CHECK(colored.find(std::string(RED()) + "--- /tmp/config" + RST()) !=
        std::string::npos);
  CHECK(colored.find(std::string(GREEN()) + "+++ /tmp/config" + RST()) !=
        std::string::npos);
  CHECK(colored.find(std::string(RED()) + "- OLD=1" + RST()) !=
        std::string::npos);
  CHECK(colored.find(std::string(GREEN()) + "+ NEW=1" + RST()) !=
        std::string::npos);
  CHECK(colored.find("  KEEP=1") != std::string::npos);

  g_color = false;
  CHECK(ColorizeDiffLines(diff) == diff);
  g_color = prior;
}

// A search the provider runs is the one long wait the status row could not
// explain: without this it reads as ordinary thinking for as many seconds as
// the search takes. The row is ephemeral on purpose -- several searches a turn
// would otherwise each leave a permanent line behind.
void TestHostedSearchStatusRow() {
  const bool prior = g_tty;
  g_tty = true;
  TerminalPresenter presenter;
  TurnStatus projection;
  auto send = [&](EventId id, json data = json::object()) {
    data["turn"] = 1;
    presenter.Consume(Event{id, data});
    if (projection.Consume(id, data)) {
      presenter.Consume(Event{EventId::kTurnStatus, projection.Status()});
    }
  };
  send(EventId::kTurnStarted);
  send(EventId::kResponseStarted);
  CHECK(CurrentTerminalStatus() == "Waiting for model");
  send(EventId::kHostedToolActivity, {{"id", "a"}, {"phase", "started"}});
  send(EventId::kHostedToolActivity, {{"id", "b"}, {"phase", "searching"}});
  send(EventId::kReasoningDelta, {{"text", "**Checking release notes**\n"}});
  CHECK(CurrentTerminalStatus() == "Searching the web");
  send(EventId::kHostedToolActivity, {{"id", "a"}, {"phase", "completed"}});
  CHECK(CurrentTerminalStatus() == "Searching the web");
  // Any other tool the provider runs is named while it runs.
  send(EventId::kHostedToolActivity,
       {{"id", "c"}, {"tool", "code_execution"}, {"phase", "started"}});
  CHECK(CurrentTerminalStatus() == "Using code_execution");
  send(EventId::kHostedToolActivity,
       {{"id", "c"}, {"tool", "code_execution"}, {"phase", "completed"}});
  CHECK(CurrentTerminalStatus() == "Searching the web");
  send(EventId::kHostedToolActivity, {{"id", "b"}, {"phase", "failed"}});
  CHECK(CurrentTerminalStatus() == "Thinking · Checking release notes");
  send(EventId::kAnswerDelta);
  CHECK(CurrentTerminalStatus().empty());
  send(EventId::kTurnStopped);
  CHECK(CurrentTerminalStatus().empty());
  const std::string corrected = CaptureStdout([&] {
    TerminalPresenter verbose;
    verbose.SetDetail(DetailFor("full"));
    verbose.Consume(AppEvent{1, "", "response.started", json::object(), false});
    verbose.Consume(AppEvent{
        2, "", "response.reasoning.delta", {{"text", "original"}}, false});
    verbose.Consume(AppEvent{3,
                             "",
                             "response.reasoning.delta",
                             {{"text", "complete reconciled snapshot"},
                              {"append_text", "revision"},
                              {"corrected", true}},
                             false});
    verbose.Finish();
  });
  CHECK(corrected.find("[Updated provider reasoning]") != std::string::npos);
  CHECK(corrected.find("revision") != std::string::npos);
  CHECK(corrected.find("complete reconciled snapshot") == std::string::npos);
  g_tty = prior;
}

// The working row used to be assembled inside the REPL loop, where nothing
// could reach it. These pin the parts that are pure arithmetic on the view:
// the frame clock, which counters appear, and the label/counter split.
void TestTurnStatusBar() {
  bool prior = g_tty;
  bool prior_color = g_color;
  g_tty = true;
  g_color = true;

  // Spinner advances one frame per 100ms and wraps after ten.
  std::string first = TurnStatusBar(Working(std::chrono::milliseconds(0)));
  std::string second = TurnStatusBar(Working(std::chrono::milliseconds(100)));
  std::string wrapped = TurnStatusBar(Working(std::chrono::milliseconds(1000)));
  CHECK(first.starts_with("⠋"));
  CHECK(second.starts_with("⠙"));
  CHECK(wrapped.starts_with("⠋"));
  CHECK(first != second);
  // Plain mode holds it still.
  g_motion = false;
  CHECK(
      TurnStatusBar(Working(std::chrono::milliseconds(100))).starts_with("⠋"));
  g_motion = true;

  // Elapsed renders at one decimal; context always shows.
  CHECK(TurnStatusBar(Working(std::chrono::milliseconds(1500))).find("1.5s") !=
        std::string::npos);
  CHECK(first.find("ctx 12k/1.3M") != std::string::npos);

  // Headroom reads as a percentage on the idle row; an unknown window has no
  // percentage to state and says nothing rather than guessing.
  CHECK(ContextLeftSummary(0, 1000) == "100% left");
  CHECK(ContextLeftSummary(250, 1000) == "75% left");
  CHECK(ContextLeftSummary(2000, 1000) == "0% left");
  CHECK(ContextLeftSummary(10, 0).empty());

  TurnStatusView unknown = Working(std::chrono::milliseconds(0));
  unknown.context_window = 0;
  std::string unknown_bar = TurnStatusBar(unknown);
  CHECK(unknown_bar.find("ctx 12k") != std::string::npos);
  CHECK(unknown_bar.find("ctx 12k/") == std::string::npos);

  // The working row names the route exactly as the idle row does, and an
  // unset route adds no segment.
  TurnStatusView routed = Working(std::chrono::milliseconds(1500));
  routed.model = "openrouter/deepseek/deepseek-v4-flash:high";
  std::string routed_bar = TurnStatusBar(routed);
  CHECK(routed_bar.find("openrouter/deepseek/deepseek-v4-flash:high") !=
        std::string::npos);
  CHECK(routed_bar.find("openrouter/deepseek/deepseek-v4-flash:high · 1.5s") !=
        std::string::npos);
  CHECK(first.find("openrouter") == std::string::npos);

  // Counters are omitted at zero rather than rendered as "act:0".
  CHECK(first.find("act:") == std::string::npos);
  CHECK(first.find("steer:") == std::string::npos);

  TurnStatusView busy = Working(std::chrono::milliseconds(0));
  busy.background = 2;
  busy.queued = 3;
  busy.foreground = 1;
  {
    FixedWidth columns(120);
    std::string loaded = TurnStatusBar(busy);
    CHECK(loaded.find("act:2") != std::string::npos);
    CHECK(loaded.find("steer:3") != std::string::npos);
    // The keys that act on the turn close the row; one foreground command
    // reads as the bare hint, more than one is counted.
    CHECK(loaded.ends_with(" · Esc stop · Ctrl+B background"));
    busy.foreground = 2;
    CHECK(TurnStatusBar(busy).find("Ctrl+B background 2 commands") !=
          std::string::npos);
  }
  // The hint is the first thing a narrow row gives up, before the counters.
  {
    FixedWidth columns(60);
    std::string narrow = TurnStatusBar(busy);
    CHECK(narrow.find("Esc stop") == std::string::npos);
    CHECK(narrow.find("steer:3") != std::string::npos);
  }

  // Delegated children get a chip of their own and lend the row their newest
  // progress line: a parent that is only waiting on a child would otherwise
  // say "Working" for as long as the child runs.
  CHECK(first.find("agents:") == std::string::npos);
  TurnStatusView delegated = Working(std::chrono::milliseconds(0));
  delegated.subagents = 2;
  delegated.subagent = "agent-1a2b3c4d: · reading";
  std::string delegated_bar = TurnStatusBar(delegated);
  CHECK(delegated_bar.find("agents:2") != std::string::npos);
  CHECK(delegated_bar.find("agent-1a2b3c4d: · reading") != std::string::npos);
  CHECK(delegated_bar.find("Working") == std::string::npos);
  // The line is another process's terminal output, so escapes must not reach
  // the row -- a child could otherwise repaint the parent's screen.
  delegated.subagent = "agent-1a2b3c4d: \033[2Jwiped";
  CHECK(TurnStatusBar(delegated).find("\033[2J") == std::string::npos);
  // The model round posts an activity of its own, and it is the one label the
  // child's progress outranks: without this the branch above is unreachable
  // for the entire time a parent spends waiting on its children.
  delegated.subagent = "agent-1a2b3c4d: · reading";
  {
    TerminalStatusLabel waiting(kWaitingStatus);
    CHECK(TurnStatusBar(delegated).find("agent-1a2b3c4d: · reading") !=
          std::string::npos);
  }
  // Anything that names actual work keeps the row.
  {
    TerminalStatusLabel running("run · make");
    std::string busy_bar = TurnStatusBar(delegated);
    CHECK(busy_bar.find("run · make") != std::string::npos);
    CHECK(busy_bar.find("agent-1a2b3c4d") == std::string::npos);
    CHECK(busy_bar.find("agents:2") != std::string::npos);
  }

  // Interrupting replaces the idle "Working" label.
  TurnStatusView interrupting = Working(std::chrono::milliseconds(0));
  interrupting.interrupting = true;
  CHECK(TurnStatusBar(interrupting).find("Interrupting") != std::string::npos);
  CHECK(first.find("Working") != std::string::npos);
  // A child's progress never displaces what this process is doing itself.
  interrupting.subagent = "agent-1a2b3c4d: · reading";
  CHECK(TurnStatusBar(interrupting).find("Interrupting") != std::string::npos);

  g_tty = prior;
  g_color = prior_color;
}

// The idle row drops segments by priority when the window is too narrow. The
// order is the whole point -- it is what decides that a user on a small
// terminal keeps the route and loses the cache hit rate rather than the other
// way round -- and until now nothing measured it.
void TestStatusBarDropsByPriority() {
  bool prior = g_tty;
  bool prior_color = g_color;
  g_tty = true;
  g_color = false;

  Usage usage;
  usage.input = 12000;
  usage.output = 3400;
  usage.cache_read = 6000;
  usage.cost = 0.42;
  StatusView view;
  view.model = "anthropic/claude-sonnet-4-5";
  view.context_used = 12000;
  view.context_window = 1300000;
  view.detail = "full";
  view.background = 1;

  // Wide enough for everything: the full row is the baseline the narrower
  // widths are read against.
  std::string wide;
  {
    FixedWidth columns(200);
    wide = StatusBar(usage, view);
  }
  CHECK(wide ==
        "anthropic/claude-sonnet-4-5 · Ask · 99% left · $0.4200 · "
        "est. ctx 12k/1.3M · act:1 · 18k in · 3.4k out · cache 33% · "
        "full · /help for shortcuts");

  // Each narrower width is a prefix of the priorities that survive: 7 (the
  // hint) goes first, then 6 (the level), then 5 (cache), and so on.
  std::string medium;
  {
    FixedWidth columns(80);
    medium = StatusBar(usage, view);
  }
  CHECK(medium.find("/help for shortcuts") == std::string::npos);
  CHECK(medium.find(" · full") == std::string::npos);
  CHECK(medium.find("anthropic/claude-sonnet-4-5") != std::string::npos);
  CHECK(DisplayWidth(medium) <= 80);
  // Route, approval mode, headroom and spend are the last to go.
  CHECK(medium.starts_with(
      "anthropic/claude-sonnet-4-5 · Ask · 99% left · $0.4200"));

  std::string narrow;
  {
    FixedWidth columns(40);
    narrow = StatusBar(usage, view);
  }
  CHECK(DisplayWidth(narrow) <= 40);
  CHECK(narrow.find("cache 33%") == std::string::npos);
  CHECK(narrow.find("18k in") == std::string::npos);

  // Priority 0 is never dropped, even when it alone overflows: the row would
  // otherwise stop saying where the request goes.
  {
    FixedWidth columns(4);
    std::string squeezed = StatusBar(usage, view);
    CHECK(squeezed == "anthropic/claude-sonnet-4-5");
  }

  g_tty = prior;
  g_color = prior_color;
}

}  // namespace uagent
