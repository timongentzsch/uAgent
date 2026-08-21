// Copyright 2026 Timon Gentzsch

#include <chrono>
#include <string>

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

Tool PollingActivityTool() {
  Tool tool;
  tool.name = "activity";
  tool.mutates = [](const json& args) {
    return args.contains("chars") || args.contains("rows");
  };
  return tool;
}

}  // namespace

// A poll costs exactly one scrollback line: "•" when nothing changed, the
// ordinary result line when it did. These pin that split and the elapsed
// clock, which only advances while an id stays quiet.
void TestPollCollapse() {
  Tool tool = PollingActivityTool();
  ToolCall call;
  call.id = "call-1";
  call.name = "activity";

  CallTask quiet;
  quiet.tool = &tool;
  quiet.args = {{"id", 4242}};
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

  // Sending input steers the activity, so it is never treated as a poll.
  CallTask steering = quiet;
  steering.args = {{"id", 4242}, {"chars", "y\n"}};
  CHECK(!IsActivityPoll(steering));
  CHECK(!ToolResultPresentation(steering, call, "", false).poll);

  // A failed poll still collapses, but is not styled as success.
  CallTask failed = quiet;
  failed.result = ToolFailure(ToolErrorCode::kNotFound, "gone");
  PresentationRecord failure = ToolResultPresentation(failed, call, "", false);
  CHECK(!failure.poll);
  CHECK(failure.status == PresentationStatus::kFailed);

  ClearPollAnchor(4242);
}

// The working row used to be assembled inside the REPL loop, where nothing
// could reach it. These pin the parts that are pure arithmetic on the view:
// the frame clock, which counters appear, and the label/counter split.
void TestActivityBar() {
  bool prior = g_tty;
  g_tty = true;

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

  ActivityView unknown = Working(std::chrono::milliseconds(0));
  unknown.context_window = 0;
  std::string unknown_bar = ActivityBar(unknown);
  CHECK(unknown_bar.find("ctx 12.0K") != std::string::npos);
  CHECK(unknown_bar.find("ctx 12.0K/") == std::string::npos);

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

  // Interrupting replaces the idle "working" label.
  ActivityView interrupting = Working(std::chrono::milliseconds(0));
  interrupting.interrupting = true;
  CHECK(ActivityBar(interrupting).find("interrupting") != std::string::npos);
  CHECK(first.find("working") != std::string::npos);

  g_tty = prior;
}

}  // namespace uagent
