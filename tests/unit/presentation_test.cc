// Copyright 2026 Timon Gentzsch

#include "include/ui/presentation.h"

#include <unistd.h>

#include <chrono>
#include <clocale>
#include <cstdio>
#include <string>
#include <utility>

#include "include/agent/child_agent.h"
#include "include/agent/session_view.h"
#include "include/cli/options.h"
#include "include/core/activity.h"
#include "include/core/defaults.h"
#include "include/core/style.h"
#include "include/core/term.h"
#include "include/tools/registry.h"
#include "include/tools/tool.h"
#include "include/ui/display.h"
#include "tests/unit/terminal_test_support.h"

namespace uagent {
namespace {

ActivityView Working(std::chrono::milliseconds elapsed) {
  ActivityView view;
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

// A tool row as the transcript view holds it.
json ToolBlock(const std::string& id, const std::string& name,
               const std::string& status, const std::string& text) {
  return {{"id", "t-" + id},
          {"kind", "tool_result"},
          {"call_id", id},
          {"name", name},
          {"status", status},
          {"text", text},
          {"activity", json::object()}};
}

}  // namespace

// A result row reads the same however its block arrived: a summary of the
// output at the default level, the output itself at the full one, the diff
// alone for a change, and the stored status, never the text, says failure.
void TestToolResultRows() {
  const std::string failure_report = ChildAgentFailureReport(
      "provider/child @ child.example", ChildAgentFailureStage::kExecution,
      "connection error: Couldn't connect to server\n"
      "api_key=child-secret-must-not-enter-row");
  auto drawn = [](const json& block, std::string_view level = "default") {
    return CaptureStdout([&] {
      TerminalPresenter presenter;
      presenter.SetDetail(DetailFor(level));
      presenter.Block(block);
    });
  };
  std::string row =
      drawn(ToolBlock("c1", "subagent", "failed", failure_report));
  CHECK(row.find("← subagent: failed: error: child execution: connection "
                 "error: Couldn't connect to server") != std::string::npos);
  CHECK(row.find("child-secret-must-not-enter-row") == std::string::npos);

  // Output is arbitrary command text. Only recorded status establishes
  // failure, and a missing receipt reads as neither.
  for (const char* status : {"success", "complete"}) {
    row = drawn(ToolBlock("c1", "run", status, "error: printed by command"));
    CHECK(row.find("← run: error: printed by command") != std::string::npos);
  }
  CHECK(drawn(ToolBlock("c1", "run", "cancelled", "stopped"))
            .find("← run: cancelled: stopped") != std::string::npos);
  CHECK(drawn(ToolBlock("c1", "run", "timed_out", "late"))
            .find("run: timed_out: late") != std::string::npos);
  CHECK(drawn(ToolBlock("c1", "read_path", "success", ""))
            .find("← read_path: (empty)") != std::string::npos);

  // A change is told entirely by its diff: no result row is drawn under it,
  // and a failed call's change was never made.
  json changed = ToolBlock("c1", "edit_file", "success", "edited a.txt");
  changed["change"] = "Edited a.txt (+1 -1)\n-old\n+new";
  row = drawn(changed);
  CHECK(row.find("Edited a.txt (+1 -1)") != std::string::npos);
  CHECK(row.find("+new") != std::string::npos);
  CHECK(row.find("←") == std::string::npos);
  changed["status"] = "failed";
  row = drawn(changed);
  CHECK(row.find("+new") == std::string::npos);
  CHECK(row.find("edit_file: failed: edited a.txt") != std::string::npos);

  // A script's output summarises as its first line and what follows; the
  // full level prints it, indented under its row.
  std::string body = "[script: .uagent/scratch/x.py · wrote]\n";
  for (int line = 1; line <= 30; ++line) body += std::to_string(line) + "\n";
  const json script = ToolBlock("c2", "scratch", "success", body);
  row = drawn(script);
  CHECK(row.find("← scratch: [script: .uagent/scratch/x.py · wrote] … · +30 "
                 "lines") != std::string::npos);
  CHECK(drawn(script, "full").find("\n    30") != std::string::npos);
  // A command's last lines stay under its row; a long result's row holds its
  // start and its end, and says only that there is more.
  json command = ToolBlock("c3", "run", "success", "1\n2\n3…");
  command["view"] = {{"output", "tail"}};
  command["truncated"] = true;
  command["tail"] = "99\n100\n";
  row = drawn(command);
  CHECK(row.find("← run: 1 … · more than ") != std::string::npos);
  CHECK(row.find("    …99\n") != std::string::npos);
  CHECK(row.find("    100\n") != std::string::npos);
  CHECK(row.find("    2\n") == std::string::npos);
  CHECK(drawn(command, "full").find("    3…\n    …99\n    100") !=
        std::string::npos);

  // A terminal whose locale cannot decode UTF-8 gets the same rows in ASCII
  // rather than mojibake. Model/tool content must remain byte-preserved.
  bool prior_unicode = g_unicode;
  g_unicode = false;
  row = drawn(script);
  CHECK(row.find("<- scratch") != std::string::npos);
  CHECK(row.find("←") == std::string::npos);
  CHECK(drawn(ToolBlock("c4", "run", "success", "model text: µ · ← …\nsecond"),
              "full")
            .find("    model text: µ · ← …\n    second") != std::string::npos);
  CHECK(StatusBarLine("thinking · 2s").find("thinking - 2s") !=
        std::string::npos);

  // Plain mode speaks each row's kind instead of drawing its glyph.
  g_plain = true;
  CHECK(drawn(ToolBlock("c1", "run", "failed", "gone")).find("failed: ") !=
        std::string::npos);
  CHECK(Note(Tone::kError, "gone") == "error: gone\n");
  CHECK(Note(Tone::kNeutral, "compacted") == "compacted\n");
  g_plain = false;
  g_unicode = prior_unicode;

  // Only an explicitly non-UTF-8 locale downgrades; an unset one is no
  // evidence that the terminal is incapable.
  const char* prior_lc_all = getenv("LC_ALL");
  setenv("LC_ALL", "C", 1);
  CHECK(!ResolveUnicodeEnabled());
  setenv("LC_ALL", "en_US.UTF-8", 1);
  CHECK(ResolveUnicodeEnabled());
  if (prior_lc_all) {
    setenv("LC_ALL", prior_lc_all, 1);
  } else {
    unsetenv("LC_ALL");
  }

  // A process started with no locale still measures conversation text.
  std::setlocale(LC_CTYPE, "C");
  CHECK(EnsureUtf8Ctype());
  CHECK(DisplayWidth("\xe4\xb8\xad") == 2);
}

// A live row arrives as its call and again once it has finished; a resumed
// one arrives finished. Each draws the call line and then the result once.
void TestToolRowsDrawLiveAsResumed() {
  json running = ToolBlock("c1", "read_path", "running", "");
  running["view"] = {{"verb", {"Reading", "Read"}}, {"target", "a.txt"}};
  running["activity"] = {{"label", "a.txt"}};
  json finished = running;
  finished["status"] = "success";
  finished["text"] = "[a.txt lines 1-3 of 3]";
  std::string live;
  const std::string resumed = CaptureStdout([&] {
    TerminalPresenter presenter;
    presenter.Block(finished);
  });
  {
    TerminalPresenter presenter;
    live = CaptureStdout([&] { presenter.Block(running); });
    // A call that has not finished has no result line.
    CHECK(live.find("→ Reading a.txt") != std::string::npos);
    CHECK(live.find("←") == std::string::npos);
    live += CaptureStdout([&] {
      presenter.Block(finished);
      presenter.Block(finished);
    });
  }
  CHECK(live == resumed);
  CHECK(resumed.find("← read_path: [a.txt lines 1-3 of 3]") !=
        std::string::npos);

  // Tool-only assistant blocks print no bare mark line, like live.
  CHECK(CaptureStdout([] {
          TerminalPresenter presenter;
          presenter.Block({{"kind", "assistant"}, {"text", ""}});
        }).empty());

  // A rejected call has no verb: it shows the arguments it sent, and a
  // multiline label (a script) is shown whole, never cut to a line.
  json rejected = running;
  rejected.erase("view");
  rejected["activity"]["label"] = R"({"path":"visible-target"})";
  const std::string script = "first\n" + std::string(4096, 'x') + "\nlast";
  json multiline = running;
  multiline["id"] = "t-c2";
  multiline["activity"]["label"] = script;
  std::string drawn = CaptureStdout([&] {
    TerminalPresenter presenter;
    presenter.Block(rejected);
    presenter.Block(multiline);
  });
  CHECK(drawn.find(R"(→ read_path({"path":"visible-target"}))") !=
        std::string::npos);
  CHECK(drawn.find("Reading\n" + script) != std::string::npos);

  // The grouped result whose id opens the group draws the group label, and
  // the rest of the group draws nothing more.
  json grouped = finished;
  grouped["activity"]["group"] = {{"id", "c1"},
                                  {"label", "Explored · 2 calls\x1b[2J"}};
  json member = grouped;
  member["id"] = "t-c2";
  member["call_id"] = "c2";
  drawn = CaptureStdout([&] {
    TerminalPresenter presenter;
    presenter.Block(grouped);
    presenter.Block(member);
  });
  CHECK(drawn.find("Explored") == drawn.rfind("Explored"));
  CHECK(drawn.find("[a.txt lines") == std::string::npos);
  // A label is stored text: shown, never obeyed.
  CHECK(drawn.find("calls\\x1b[2J") != std::string::npos);
}

// Every client renders one vocabulary; the shapes below are that contract.
// One recorded turn at the three levels: a turn's work in one row, in rows
// per group, or every call with its output. A failure always keeps its row.
void TestVerbosityLevels() {
  auto result = [](const char* id, const char* status, const char* category,
                   json group = nullptr) {
    json block = ToolBlock(id, "read_path", status,
                           std::string("body of ") + id + "\nsecond line");
    block["view"] = {{"verb", {"Reading", "Read"}},
                     {"target", std::string(id) + ".txt"}};
    block["activity"] = {{"category", category}};
    if (!group.is_null()) block["activity"]["group"] = std::move(group);
    return block;
  };
  const json group = {{"id", "a"}, {"label", "Explored · 2 calls"}};
  auto drawn = [&](std::string_view level) {
    return CaptureStdout([&] {
      TerminalPresenter presenter;
      presenter.SetDetail(DetailFor(level));
      presenter.Block({{"kind", "user"}, {"text", "go"}});
      presenter.Block(
          {{"kind", "assistant"}, {"text", "done"}, {"reasoning", "pondered"}});
      presenter.Block(result("a", "success", "explore", group));
      presenter.Block(result("b", "success", "explore", group));
      presenter.Block(result("c", "success", "edit"));
      presenter.Block(result("d", "failed", "explore"));
      presenter.Block({{"kind", "turn_summary"}, {"summary", json::object()}});
    });
  };
  const std::string minimal = drawn("minimal");
  CHECK(minimal.find("Worked · 3 steps · edited 1 file\n") !=
        std::string::npos);
  CHECK(minimal.find("body of a") == std::string::npos);
  CHECK(minimal.find("Reading") == std::string::npos);
  CHECK(minimal.find("Explored") == std::string::npos);
  CHECK(minimal.find("body of d") != std::string::npos);
  CHECK(minimal.find("pondered") == std::string::npos);
  CHECK(minimal.find("done") != std::string::npos);

  const std::string standard = drawn("default");
  CHECK(standard.find("Worked") == std::string::npos);
  CHECK(standard.find("→ Reading a.txt") != std::string::npos);
  CHECK(standard.find("Explored · 2 calls") != std::string::npos);
  CHECK(standard.find("body of a") == std::string::npos);
  CHECK(standard.find("body of c … · +1 lines") != std::string::npos);
  CHECK(standard.find("pondered") == std::string::npos);
  // An unknown level reads as the default one.
  CHECK(drawn("loud") == standard);

  const std::string full = drawn("full");
  CHECK(full.find("Explored") == std::string::npos);
  CHECK(full.find("    body of a\n    second line") != std::string::npos);
  CHECK(full.find("Thinking") != std::string::npos);
  CHECK(full.find("pondered") != std::string::npos);
}

void TestToolViews() {
  // The generic view: short scalars are fields, long text is code, nested
  // values are indented JSON, and display labels are never repeated.
  json view = ToolView(nullptr, {{"path", "a.txt"},
                                 {"limit", 20},
                                 {"intent", "read"},
                                 {"body", "one\ntwo"},
                                 {"edits", json::array({{{"old", "x"}}})}});
  CHECK(view["output"] == "text");
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
}

// The resume hint is for the model; rows link to the agent through facts.
void TestModelHints() {
  CHECK(StripModelHints("done\n[collaborator agent-1; resume with subagent "
                        "operation=followup]") == "done");
  CHECK(StripModelHints("done\n[collaborator agent-1; persistent runtime "
                        "retained]") == "done");
  // Only a final line is a trailer; the same text elsewhere is content.
  CHECK(StripModelHints("[collaborator x]\nmore") == "[collaborator x]\nmore");
  // A started-work hint gives way to the output; the row links the work.
  CHECK(StripModelHints("[running] activity 7; poll or wait\nServing") ==
        "Serving");
  CHECK(StripModelHints("[started] subagent id 7; wait").empty());
  CHECK(StripModelHints("plain") == "plain");
}

// Each marker a tool writes for the model is stripped from the view, and the
// parser reads exactly what the writer emits.
void TestModelHintMarkers() {
  for (const char* marker :
       {kRunningActivityMarker, kStartedSubagentMarker, kDetachedMarker}) {
    CHECK(StripModelHints(std::string(marker) + "7; hint\nbody") == "body");
    CHECK(StripModelHints("body " + std::string(marker) + "7") ==
          "body " + std::string(marker) + "7");
  }
  CHECK(StripModelHints("done" + ChildAgentResumeNote("agent-1")) == "done");
  const std::string hint = ArtifactHint({"/tmp/log.txt", 99});
  CHECK(hint.starts_with(kCapturedLogMarker));
  CHECK(StripArtifactHint("tail line" + hint) == "tail line");
  CHECK(StripArtifactHint("tail line") == "tail line");
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
  ActivityProjection projection;
  auto send = [&](EventId id, json data = json::object()) {
    data["turn"] = 1;
    Event event{id, data};
    event.render = true;
    presenter.Consume(event);
    if (projection.Consume(id, data)) {
      Event status{EventId::kActivityStatus, projection.Status()};
      status.render = true;
      presenter.Consume(status);
    }
  };
  send(EventId::kTurnStarted);
  send(EventId::kResponseStarted);
  CHECK(CurrentTerminalActivity() == "Waiting for model");
  send(EventId::kHostedToolActivity, {{"id", "a"}, {"phase", "started"}});
  send(EventId::kHostedToolActivity, {{"id", "b"}, {"phase", "searching"}});
  send(EventId::kReasoningDelta, {{"text", "**Checking release notes**\n"}});
  CHECK(CurrentTerminalActivity() == "Searching the web");
  send(EventId::kHostedToolActivity, {{"id", "a"}, {"phase", "completed"}});
  CHECK(CurrentTerminalActivity() == "Searching the web");
  // Any other tool the provider runs is named while it runs.
  send(EventId::kHostedToolActivity,
       {{"id", "c"}, {"tool", "code_execution"}, {"phase", "started"}});
  CHECK(CurrentTerminalActivity() == "Using code_execution");
  send(EventId::kHostedToolActivity,
       {{"id", "c"}, {"tool", "code_execution"}, {"phase", "completed"}});
  CHECK(CurrentTerminalActivity() == "Searching the web");
  send(EventId::kHostedToolActivity, {{"id", "b"}, {"phase", "failed"}});
  CHECK(CurrentTerminalActivity() == "Thinking · Checking release notes");
  send(EventId::kAnswerDelta);
  CHECK(CurrentTerminalActivity().empty());
  send(EventId::kTurnStopped);
  CHECK(CurrentTerminalActivity().empty());
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
void TestActivityBar() {
  bool prior = g_tty;
  bool prior_color = g_color;
  g_tty = true;
  g_color = true;

  // Spinner advances one frame per 100ms and wraps after ten.
  std::string first = ActivityBar(Working(std::chrono::milliseconds(0)));
  std::string second = ActivityBar(Working(std::chrono::milliseconds(100)));
  std::string wrapped = ActivityBar(Working(std::chrono::milliseconds(1000)));
  CHECK(first.starts_with("⠋"));
  CHECK(second.starts_with("⠙"));
  CHECK(wrapped.starts_with("⠋"));
  CHECK(first != second);
  // Reduced motion holds it still.
  g_motion = false;
  CHECK(ActivityBar(Working(std::chrono::milliseconds(100))).starts_with("⠋"));
  g_motion = true;

  // Elapsed renders at one decimal; context always shows.
  CHECK(ActivityBar(Working(std::chrono::milliseconds(1500))).find("1.5s") !=
        std::string::npos);
  CHECK(first.find("ctx 12k/1.3M") != std::string::npos);

  // Headroom reads as a percentage on the idle row; an unknown window has no
  // percentage to state and says nothing rather than guessing.
  CHECK(ContextLeftSummary(0, 1000) == "100% left");
  CHECK(ContextLeftSummary(250, 1000) == "75% left");
  CHECK(ContextLeftSummary(2000, 1000) == "0% left");
  CHECK(ContextLeftSummary(10, 0).empty());

  ActivityView unknown = Working(std::chrono::milliseconds(0));
  unknown.context_window = 0;
  std::string unknown_bar = ActivityBar(unknown);
  CHECK(unknown_bar.find("ctx 12k") != std::string::npos);
  CHECK(unknown_bar.find("ctx 12k/") == std::string::npos);

  // The working row names the route exactly as the idle row does, and an
  // unset route adds no segment.
  ActivityView routed = Working(std::chrono::milliseconds(1500));
  routed.model = "openrouter/deepseek/deepseek-v4-flash:high";
  std::string routed_bar = ActivityBar(routed);
  CHECK(routed_bar.find("openrouter/deepseek/deepseek-v4-flash:high") !=
        std::string::npos);
  CHECK(routed_bar.find("openrouter/deepseek/deepseek-v4-flash:high · 1.5s") !=
        std::string::npos);
  CHECK(first.find("openrouter") == std::string::npos);

  // Counters are omitted at zero rather than rendered as "bg:0".
  CHECK(first.find("bg:") == std::string::npos);
  CHECK(first.find("steer:") == std::string::npos);

  ActivityView busy = Working(std::chrono::milliseconds(0));
  busy.background = 2;
  busy.queued = 3;
  busy.foreground = 1;
  {
    FixedWidth columns(120);
    std::string loaded = ActivityBar(busy);
    CHECK(loaded.find("bg:2") != std::string::npos);
    CHECK(loaded.find("steer:3") != std::string::npos);
    // The keys that act on the turn close the row; one foreground command
    // reads as the bare hint, more than one is counted.
    CHECK(loaded.ends_with(" · Esc stop · Ctrl+B background"));
    busy.foreground = 2;
    CHECK(ActivityBar(busy).find("Ctrl+B background 2 commands") !=
          std::string::npos);
  }
  // The hint is the first thing a narrow row gives up, before the counters.
  {
    FixedWidth columns(60);
    std::string narrow = ActivityBar(busy);
    CHECK(narrow.find("Esc stop") == std::string::npos);
    CHECK(narrow.find("steer:3") != std::string::npos);
  }

  // Delegated children get a chip of their own and lend the row their newest
  // progress line: a parent that is only waiting on a child would otherwise
  // say "Working" for as long as the child runs.
  CHECK(first.find("agents:") == std::string::npos);
  ActivityView delegated = Working(std::chrono::milliseconds(0));
  delegated.subagents = 2;
  delegated.subagent = "agent-1a2b3c4d: · reading";
  std::string delegated_bar = ActivityBar(delegated);
  CHECK(delegated_bar.find("agents:2") != std::string::npos);
  CHECK(delegated_bar.find("agent-1a2b3c4d: · reading") != std::string::npos);
  CHECK(delegated_bar.find("Working") == std::string::npos);
  // The line is another process's terminal output, so escapes must not reach
  // the row -- a child could otherwise repaint the parent's screen.
  delegated.subagent = "agent-1a2b3c4d: \033[2Jwiped";
  CHECK(ActivityBar(delegated).find("\033[2J") == std::string::npos);
  // The model round posts an activity of its own, and it is the one label the
  // child's progress outranks: without this the branch above is unreachable
  // for the entire time a parent spends waiting on its children.
  delegated.subagent = "agent-1a2b3c4d: · reading";
  {
    TerminalActivityLabel waiting(kWaitingActivity);
    CHECK(ActivityBar(delegated).find("agent-1a2b3c4d: · reading") !=
          std::string::npos);
  }
  // Anything that names actual work keeps the row.
  {
    TerminalActivityLabel running("run · make");
    std::string busy_bar = ActivityBar(delegated);
    CHECK(busy_bar.find("run · make") != std::string::npos);
    CHECK(busy_bar.find("agent-1a2b3c4d") == std::string::npos);
    CHECK(busy_bar.find("agents:2") != std::string::npos);
  }

  // Interrupting replaces the idle "Working" label.
  ActivityView interrupting = Working(std::chrono::milliseconds(0));
  interrupting.interrupting = true;
  CHECK(ActivityBar(interrupting).find("Interrupting") != std::string::npos);
  CHECK(first.find("Working") != std::string::npos);
  // A child's progress never displaces what this process is doing itself.
  interrupting.subagent = "agent-1a2b3c4d: · reading";
  CHECK(ActivityBar(interrupting).find("Interrupting") != std::string::npos);

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
  view.verbosity = "full";
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
        "est. ctx 12k/1.3M · bg:1 · 18k in · 3.4k out · cache 33% · "
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
