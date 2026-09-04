// Copyright 2026 Timon Gentzsch

#include "include/ui/presentation.h"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <string>

#include "include/core/term.h"
#include "include/tools/child_agent.h"
#include "include/ui/display.h"
#include "include/ui/tool_output.h"
#include "tests/unit/test_support.h"

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

Tool PollingActivityTool() {
  Tool tool;
  tool.name = "activity";
  tool.mutates = [](const json& args) {
    return JsonValue(args, "operation", "") == "write" ||
           JsonValue(args, "operation", "") == "resize";
  };
  return tool;
}

}  // namespace

// A poll costs exactly one scrollback line: "•" when nothing changed, the
// ordinary result line when it did. These pin that split and the elapsed
// clock, which only advances while an id stays quiet.
void TestPollCollapse() {
  const std::string failure_report = ChildAgentFailureReport(
      "provider/child @ child.example", ChildAgentFailureStage::kExecution,
      "connection error: Couldn't connect to server\n"
      "api_key=child-secret-must-not-enter-row");
  PresentationRecord failed_child;
  failed_child.kind = PresentationKind::kToolResult;
  failed_child.status = PresentationStatus::kFailed;
  failed_child.title = "subagent 1073741866";
  failed_child.summary = FirstLine(failure_report);
  std::string failed_row =
      CaptureStdout([&] { PrintPresentation(failed_child); });
  CHECK(failed_row.find(
            "← subagent 1073741866: error: child execution: connection "
            "error: Couldn't connect to server") != std::string::npos);
  CHECK(failed_row.find("delegated child failed") == std::string::npos);
  CHECK(failed_row.find("child-secret-must-not-enter-row") ==
        std::string::npos);

  Tool tool = PollingActivityTool();
  ToolCall call;
  call.id = "call-1";
  call.name = "activity";

  CallTask quiet;
  quiet.tool = &tool;
  quiet.args = {{"operation", "poll"}, {"id", 4242}};
  quiet.ordinal = "[1] ";
  quiet.result = ToolSuccess("(no new output)");
  quiet.result.no_change = true;

  CHECK(IsActivityPoll(quiet));
  ClearPollAnchor(4242);

  // The call line is withheld; the outcome is unknown until the result.
  PresentationRecord call_record = ToolCallPresentation(quiet, call);
  CHECK(call_record.poll);

  PresentationRecord first = ToolResultPresentation(quiet, call, "", false);
  CHECK(first.poll);
  CHECK(first.summary.find("waited on activity 4242") != std::string::npos);
  CHECK(first.detail.empty());

  // A second quiet poll keeps the original anchor, so elapsed only grows.
  auto before = PollElapsed(4242);
  PresentationRecord second = ToolResultPresentation(quiet, call, "", false);
  CHECK(second.poll);
  CHECK(PollElapsed(4242) >= before);

  // Real output ends the quiet spell and renders as an ordinary result.
  CallTask productive = quiet;
  productive.result = ToolSuccess("server ready");
  PresentationRecord shown =
      ToolResultPresentation(productive, call, "", false);
  CHECK(!shown.poll);
  CHECK(shown.summary.find("waited on activity") == std::string::npos);

  // Call arguments are history, not bounded result output. A long multiline
  // call must survive both the live and replay presentation record intact.
  std::string long_call = "first\n" + std::string(4096, 'x') + "\nlast";
  PresentationRecord complete;
  complete.kind = PresentationKind::kToolCall;
  SetCallLabel(complete, long_call);
  CHECK(complete.multiline);
  CHECK(complete.detail == long_call);

  // Sending input steers the activity, so it is never treated as a poll.
  CallTask steering = quiet;
  steering.args = {{"operation", "write"}, {"id", 4242}, {"chars", "y\n"}};
  CHECK(!IsActivityPoll(steering));
  CHECK(!ToolResultPresentation(steering, call, "", false).poll);

  // A failed poll still collapses, but is not styled as success.
  CallTask failed = quiet;
  failed.result = ToolFailure(ToolErrorCode::kNotFound, "gone");
  PresentationRecord failure = ToolResultPresentation(failed, call, "", false);
  CHECK(!failure.poll);
  CHECK(failure.status == PresentationStatus::kFailed);

  // A resumed transcript stores the text the model saw, not the status the
  // live row was coloured from, so replay reads the `error: ` prefix rather
  // than repainting every past failure as a success.
  CHECK(StoredToolResultPresentation("run", "error: no such file").status ==
        PresentationStatus::kFailed);
  CHECK(StoredToolResultPresentation("run", "built 3 targets").status ==
        PresentationStatus::kSucceeded);
  // A body that merely mentions an error later is still a success.
  CHECK(StoredToolResultPresentation("run", "cc a.c\nerror: bad").status ==
        PresentationStatus::kSucceeded);
  // A kept receipt replays as it was drawn rather than as a summary line, so
  // a resumed diff keeps the colour it had when it happened.
  PresentationRecord redrawn =
      StoredToolResultPresentation("edit_file", "edited a.txt", "-old\n+new");
  CHECK(redrawn.change == "-old\n+new");
  // A file write is fully told by its diff: there is no output row under it.
  CHECK(redrawn.detail.empty());
  CHECK(!redrawn.multiline);
  // A script that was written and then run owes the person what it printed,
  // so the receipt and the output are both kept.
  PresentationRecord ran = StoredToolResultPresentation(
      "scratch", "[script: .uagent/scratch/x.py · wrote · executed]\n42\n",
      "x.py\n+print(6*7)");
  CHECK(ran.change == "x.py\n+print(6*7)");
  CHECK(ran.multiline);
  CHECK(ran.detail == "42");
  // A failure is still a failure, receipt or not.
  CHECK(StoredToolResultPresentation("edit_file", "error: no such file", "x")
            .status == PresentationStatus::kFailed);

  // A script that was written and then run renders both: the receipt above,
  // the bounded output below. Without this the terminal showed the code that
  // ran and never what it returned.
  bool tty = g_tty;
  g_tty = true;
  CallTask script;
  script.tool = &tool;
  script.args = json::object();
  script.ordinal = "[2] ";
  std::string body;
  for (int line = 1; line <= 30; ++line) body += std::to_string(line) + "\n";
  script.result = ToolSuccess("[script: .uagent/scratch/x.py · wrote]\n" + body);
  script.result.display = "Created x.py\n+print(1)";
  PresentationRecord compact =
      ToolResultPresentation(script, call, script.result.output, false);
  CHECK(compact.change == "Created x.py\n+print(1)");
  CHECK(compact.multiline);
  CHECK(compact.detail.starts_with("1\n2\n"));
  CHECK(compact.detail.find("30 lines") != std::string::npos);
  CHECK(compact.detail.find("\n30\n") == std::string::npos);  // bounded
  PresentationRecord loud =
      ToolResultPresentation(script, call, script.result.output, true);
  CHECK(loud.change == compact.change);
  CHECK(loud.detail.find("\n30") != std::string::npos);  // /verbose is whole
  std::string drawn = CaptureStdout([&] { PrintPresentation(compact); });
  CHECK(drawn.find("Created x.py") != std::string::npos);
  CHECK(drawn.find("+print(1)") != std::string::npos);
  CHECK(drawn.find("← [2] activity") != std::string::npos);
  CHECK(drawn.find("\n1\n") != std::string::npos);
  // A file write is told entirely by its diff: no empty row is drawn under it.
  CallTask wrote = script;
  wrote.result = ToolSuccess("wrote 9 bytes to a.txt");
  wrote.result.display = "Created a.txt\n+x";
  PresentationRecord receipt =
      ToolResultPresentation(wrote, call, wrote.result.output, false);
  CHECK(receipt.detail.empty() && receipt.summary.empty());
  CHECK(CaptureStdout([&] { PrintPresentation(receipt); }).find("←") ==
        std::string::npos);
  // Without a terminal the row summarises the output, not the receipt line.
  g_tty = false;
  PresentationRecord headless =
      ToolResultPresentation(script, call, script.result.output, false);
  CHECK(headless.change.empty());
  CHECK(headless.summary.find("[script:") == std::string::npos);
  CHECK(headless.summary.find("1") != std::string::npos);
  g_tty = tty;

  ClearPollAnchor(4242);
}

// A search the provider runs is the one long wait the status row could not
// explain: without this it reads as ordinary thinking for as many seconds as
// the search takes. The row is ephemeral on purpose -- several searches a turn
// would otherwise each leave a permanent line behind.
void TestHostedSearchStatusRow() {
  bool prior = g_tty;
  FixedWidth columns(80);
  g_tty = true;

  TerminalPresenter presenter;
  auto respond = [&] {
    Event started{EventId::kResponseStarted};
    started.render = true;
    started.text = kWaitingActivity;
    presenter.Consume(started);
  };
  auto search = [&](const char* id, const char* phase) {
    presenter.Consume(
        Event{EventId::kHostedToolActivity,
              {{"tool", "web_search"}, {"id", id}, {"phase", phase}}});
  };
  auto think = [&](std::string_view text) {
    Event delta{EventId::kReasoningDelta};
    delta.text = text;
    presenter.Consume(delta);
  };

  // Working -> searching the web -> thinking -> answer.
  respond();
  CHECK(CurrentTerminalActivity() == kWaitingActivity);
  search("ws_1", "started");
  CHECK(CurrentTerminalActivity() == "searching the web");
  CHECK(!CurrentTerminalActivityRolling());
  // Reasoning arriving mid-search grows the buffer without taking the row.
  think("checking the release notes");
  CHECK(CurrentTerminalActivity() == "searching the web");
  search("ws_1", "completed");
  CHECK(CurrentTerminalActivityRolling());
  Event answer{EventId::kAnswerDelta};
  answer.text = "done";
  presenter.Consume(answer);
  CHECK(CurrentTerminalActivity().empty());
  presenter.Consume(Event{EventId::kResponseFinished});

  // A search opening after reasoning takes the row back, and hands it to the
  // ticker rather than to the base label when it ends.
  respond();
  think("weighing options");
  CHECK(CurrentTerminalActivityRolling());
  search("ws_2", "searching");
  CHECK(CurrentTerminalActivity() == "searching the web");
  search("ws_2", "completed");
  CHECK(CurrentTerminalActivityRolling());
  presenter.Consume(Event{EventId::kResponseFinished});

  // Overlapping searches share the row; only the last to finish releases it.
  respond();
  search("ws_3", "started");
  search("ws_4", "started");
  search("ws_3", "completed");
  CHECK(CurrentTerminalActivity() == "searching the web");
  search("ws_4", "failed");
  CHECK(CurrentTerminalActivity() == kWaitingActivity);

  // A completion for a search that never opened cannot strand the row.
  search("ws_5", "completed");
  CHECK(CurrentTerminalActivity() == kWaitingActivity);
  presenter.Consume(Event{EventId::kResponseFinished});

  // A response that ends mid-search stops the row with it: nothing outlives
  // the response that owns it.
  respond();
  search("ws_6", "searching");
  CHECK(CurrentTerminalActivity() == "searching the web");
  presenter.Consume(Event{EventId::kResponseFinished});
  CHECK(CurrentTerminalActivity().empty());

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

  // Elapsed renders at one decimal; context always shows.
  CHECK(ActivityBar(Working(std::chrono::milliseconds(1500))).find("1.5s") !=
        std::string::npos);
  CHECK(first.find("ctx 12.0K/1.3M") != std::string::npos);

  // Headroom reads as a percentage on the idle row; an unknown window has no
  // percentage to state and says nothing rather than guessing.
  CHECK(ContextLeftSummary(0, 1000) == "100% left");
  CHECK(ContextLeftSummary(250, 1000) == "75% left");
  CHECK(ContextLeftSummary(2000, 1000) == "0% left");
  CHECK(ContextLeftSummary(10, 0).empty());

  ActivityView unknown = Working(std::chrono::milliseconds(0));
  unknown.context_window = 0;
  std::string unknown_bar = ActivityBar(unknown);
  CHECK(unknown_bar.find("ctx 12.0K") != std::string::npos);
  CHECK(unknown_bar.find("ctx 12.0K/") == std::string::npos);

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
  CHECK(first.find("Ctrl+B") == std::string::npos);

  ActivityView busy = Working(std::chrono::milliseconds(0));
  busy.background = 2;
  busy.queued = 3;
  busy.foreground = 1;
  std::string loaded = ActivityBar(busy);
  CHECK(loaded.find("bg:2") != std::string::npos);
  CHECK(loaded.find("steer:3") != std::string::npos);
  CHECK(loaded.find("Ctrl+B background") != std::string::npos);
  // One foreground command reads as a bare hint; more than one is counted.
  CHECK(loaded.find("commands") == std::string::npos);
  busy.foreground = 2;
  CHECK(ActivityBar(busy).find("2 commands") != std::string::npos);

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

  Api api{RuntimeConfig{}};
  api.ctx_window = 1300000;
  Usage usage;
  usage.input = 12000;
  usage.output = 3400;
  usage.cache_read = 6000;
  usage.cost = 0.42;
  StatusView view;
  view.model = "anthropic/claude-sonnet-4-5";
  view.context_used = 12000;
  view.verbose = true;
  view.attachments = 2;
  view.background = 1;

  // Wide enough for everything: the full row is the baseline the narrower
  // widths are read against.
  std::string wide;
  {
    FixedWidth columns(200);
    wide = StatusBar(api, usage, view);
  }
  CHECK(wide ==
        "anthropic/claude-sonnet-4-5 · ctx 12.0K/1.3M · 99% left · "
        "12.0K in · 3.4K out · cache 33% · $0.4200 · bg:1 · 2 attached · "
        "verbose · /help for shortcuts");

  // Each narrower width is a prefix of the priorities that survive: 7 (the
  // hint) goes first, then 6 (verbose), then 5 (cache), and so on.
  std::string medium;
  {
    FixedWidth columns(80);
    medium = StatusBar(api, usage, view);
  }
  CHECK(medium.find("/help for shortcuts") == std::string::npos);
  CHECK(medium.find("verbose") == std::string::npos);
  CHECK(medium.find("anthropic/claude-sonnet-4-5") != std::string::npos);
  CHECK(DisplayWidth(medium) <= 80);

  std::string narrow;
  {
    FixedWidth columns(40);
    narrow = StatusBar(api, usage, view);
  }
  CHECK(DisplayWidth(narrow) <= 40);
  CHECK(narrow.find("cache 33%") == std::string::npos);
  CHECK(narrow.find("12.0K in") == std::string::npos);

  // Priority 0 is never dropped, even when it alone overflows: the row would
  // otherwise stop saying where the request goes.
  {
    FixedWidth columns(4);
    std::string squeezed = StatusBar(api, usage, view);
    CHECK(squeezed == "anthropic/claude-sonnet-4-5");
  }

  // A resolved provider scope is the whole of segment 0; an unresolvable one
  // appends the host, and that pair still cannot be split apart.
  {
    FixedWidth columns(4);
    StatusView hosted = view;
    hosted.model = "local-model";
    hosted.host = "127.0.0.1";
    CHECK(StatusBar(api, usage, hosted) == "local-model @ 127.0.0.1");
  }

  g_tty = prior;
  g_color = prior_color;
}

}  // namespace uagent
