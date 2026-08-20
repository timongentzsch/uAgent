// Copyright 2026 Timon Gentzsch

#include <chrono>
#include <string>

#include "include/ui/display.h"
#include "tests/unit/test_support.h"

namespace uagent {
namespace {

ActivityView Working(std::chrono::milliseconds elapsed) {
  ActivityView view;
  view.elapsed = elapsed;
  view.context_used = 12000;
  return view;
}

}  // namespace

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
  CHECK(first.find("ctx 12.0K") != std::string::npos);

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
