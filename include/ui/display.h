// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_DISPLAY_H_
#define UAGENT_INCLUDE_UI_DISPLAY_H_
// The terminal's status rows and the summaries they are made of.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "include/core/detail.h"
#include "include/core/env.h"
#include "include/core/strings.h"
#include "include/core/style.h"
#include "include/core/term.h"
#include "include/core/usage.h"

namespace uagent {

// The whole prompt in, everything generated out; cache is separate so a
// narrow terminal can drop it on its own.
inline std::string TokenSummary(const Usage& usage) {
  return FmtCount(usage.PromptTokens()) + " in · " +
         FmtCount(usage.GeneratedTokens()) + " out";
}

// Every bucket, as a turn's footer and /cost spell them.
inline std::string TokenSplit(const Usage& usage) {
  std::string line = FmtCount(usage.input) + " in";
  if (usage.cache_read) line += " (+" + FmtCount(usage.cache_read) + " cached)";
  if (usage.cache_write) {
    line += " (+" + FmtCount(usage.cache_write) + " cache write)";
  }
  line += " · " + FmtCount(usage.output) + " out";
  if (usage.reasoning) {
    line += " (+" + FmtCount(usage.reasoning) + " reasoning)";
  }
  return line;
}

inline std::string CacheSummary(const Usage& usage) {
  return usage.cache_read
             ? "cache " + std::to_string(usage.CacheHitPercent()) + "%"
             : std::string();
}

// Headroom as a percentage; empty when the window is unknown.
inline std::string ContextLeftSummary(int64_t used, int64_t window) {
  if (window <= 0) return {};
  int64_t left = std::clamp<int64_t>(window - used, 0, window);
  return std::to_string(left * 100 / window) + "% left";
}

inline std::string ContextSummary(int64_t used, int64_t window = 0) {
  std::string context = "est. ctx " + FmtCount(used);
  if (window > 0) context += "/" + FmtCount(window);
  return context;
}

// The idle status row's content; a running turn has TurnStatusView.
struct StatusView {
  // What the session is doing between turns: Ready, Interrupted, Connecting.
  std::string turn_status{};
  int64_t context_used = 0;
  int64_t context_window = 0;
  // The active route as a selection the user could paste back into --model.
  std::string model{};
  // The effective approval mode, as ApprovalModeName spells it.
  std::string approval = "ask";
  // The detail level, named in the row when it is not the one a new install
  // starts at.
  std::string detail{kDetailLevels[0]};
  size_t background = 0;
};

// One ordered list of segments, dropped by priority when the terminal is
// too narrow.
inline std::string StatusBar(const Usage& usage, const StatusView& view) {
  struct Segment {
    int priority;  // higher is dropped first
    std::string text;
  };
  std::vector<Segment> segments;
  auto add = [&segments](int priority, const std::string& text) {
    if (!text.empty()) segments.push_back({priority, text});
  };

  add(0, view.turn_status);
  add(0, view.model);
  add(1, view.approval == "yolo"   ? "YOLO"
         : view.approval == "auto" ? "Auto"
         : view.approval == "plan" ? "Plan"
                                   : "Ask");
  add(1, ContextLeftSummary(view.context_used, view.context_window));
  if (usage.cost > 0) add(2, FmtCost(usage.cost));
  add(3, ContextSummary(view.context_used, view.context_window));
  if (view.background) {
    add(3, "act:" + FmtCount(static_cast<int64_t>(view.background)));
  }
  if (usage.PromptTokens() || usage.GeneratedTokens()) {
    add(4, TokenSummary(usage));
  }
  add(5, CacheSummary(usage));
  if (view.detail != kDetailLevels[0]) add(6, view.detail);
  add(7, "/help for shortcuts");

  auto join = [&segments] {
    std::string line;
    for (const Segment& segment : segments) {
      if (!line.empty()) line += " · ";
      line += segment.text;
    }
    return line;
  };
  std::string line = join();
  if (!g_tty) return line;
  // The least valuable segment goes first, the rightmost of a tie.
  while (DisplayWidth(line) > TerminalWidth(1) && segments.size() > 1) {
    auto victim = std::max_element(segments.rbegin(), segments.rend(),
                                   [](const Segment& a, const Segment& b) {
                                     return a.priority < b.priority;
                                   });
    if (victim->priority == 0) break;
    segments.erase(std::next(victim).base());
    line = join();
  }
  return line;
}

// Work state for the pinned row while a turn runs. It is handed the frame
// clock instead of reading it, so a test can drive a frame.
struct TurnStatusView {
  std::chrono::steady_clock::duration elapsed{};
  int64_t context_used = 0;
  int64_t context_window = 0;
  // The active route, spelled as StatusView::model.
  std::string model{};
  size_t background = 0;
  size_t foreground = 0;
  size_t queued = 0;
  bool interrupting = false;
};

// The working row. When it does not fit, the label is cut: the counters on
// the right are the part that changes.
inline std::string TurnStatusBar(const TurnStatusView& view) {
  std::string seconds =
      FmtDuration(std::chrono::duration<double>(view.elapsed).count());
  std::string status = CurrentTerminalStatus();
  // Without motion the spinner holds still.
  auto ticks = g_motion ? view.elapsed / kSpinnerPeriod : 0;
  std::string prefix = SpinnerFrame(static_cast<size_t>(ticks));
  prefix += " ";
  std::string state = status.empty() ? std::string(kWaitingStatus) : status;
  if (view.interrupting) state = "Interrupting";
  auto counted = [](const char* label, size_t count) {
    return count ? label + FmtCount(static_cast<int64_t>(count)) : "";
  };
  std::string suffix =
      " · " +
      JoinDot({view.model, seconds,
               ContextSummary(view.context_used, view.context_window),
               counted("act:", view.background),
               counted("steer:", view.queued)});
  // The hint is the first thing a narrow terminal gives up.
  std::string hint = " · Esc stop · Ctrl+B background";
  if (view.foreground > 1) {
    hint += " " + FmtCount(static_cast<int64_t>(view.foreground)) + " commands";
  }
  size_t width = TerminalWidth(1);
  size_t desired = std::min<size_t>(DisplayWidth(state), 64);
  if (DisplayWidth(prefix) + DisplayWidth(suffix) + DisplayWidth(hint) +
          desired <=
      width) {
    suffix += hint;
  }
  size_t reserved = DisplayWidth(prefix) + DisplayWidth(suffix);
  size_t status_width = width > reserved ? width - reserved : 0;
  return prefix + StatusLabel(state, status_width) + suffix;
}

// Dim, clipped to the terminal, and cleared to the right so a shorter line
// leaves no stale text behind.
inline std::string StatusBarLine(const std::string& status) {
  std::string text =
      DisplayTrunc(AsciiGlyphs(TerminalSafe(status)), TerminalWidth(1));
  return std::string(RST()) + DIM() + text + kEraseRight + RST();
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_DISPLAY_H_
