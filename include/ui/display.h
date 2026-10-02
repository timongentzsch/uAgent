// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_DISPLAY_H_
#define UAGENT_INCLUDE_UI_DISPLAY_H_
// Terminal rendering for the REPL: model/route listings and the status line.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/strings.h"
#include "include/core/style.h"
#include "include/core/term.h"
#include "include/core/usage.h"

namespace uagent {

// Token counts as the status row and /cost both spell them. Cache is separate
// so the status row can drop it independently when the terminal is narrow.
inline std::string TokenSummary(const Usage& usage) {
  return FmtCount(usage.input) + " in · " + FmtCount(usage.output) + " out";
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

// Compact session metadata for the persistent composer. Keep the stable
// identity first; transient work state gets its own line while a turn runs.
struct StatusView {
  // What the session is doing between turns: Ready, Interrupted, Connecting.
  std::string activity{};
  int64_t context_used = 0;
  int64_t context_window = 0;
  // The active route in schema form, [provider/]model[:variant][:effort], so
  // the row shows a selection the user could paste back into --model.
  std::string model{};
  // The effective approval mode, as ApprovalModeName spells it.
  std::string approval = "ask";
  bool verbose = false;
  size_t background = 0;
};

// One ordered list of segments, rendered in place and dropped by priority when
// the terminal is too narrow. A second hand-maintained "cramped" spelling of
// the same row is how the two copies used to drift.
inline std::string StatusBar(const Usage& usage, const StatusView& view) {
  struct Segment {
    int priority;  // higher is dropped first
    std::string text;
  };
  std::vector<Segment> segments;
  auto add = [&segments](int priority, std::string text) {
    if (!text.empty()) segments.push_back({priority, std::move(text)});
  };

  // Where requests go, what they may do unasked, how much room is left and
  // what it has cost lead the row and are the last to go.
  add(0, view.activity);
  add(0, view.model);
  add(1, view.approval == "yolo"   ? "YOLO"
         : view.approval == "auto" ? "Auto"
                                   : "Ask");
  add(1, ContextLeftSummary(view.context_used, view.context_window));
  if (usage.cost > 0) add(2, FmtCost(usage.cost));
  add(3, ContextSummary(view.context_used, view.context_window));
  if (view.background) {
    add(3, "bg:" + FmtCount(static_cast<int64_t>(view.background)));
  }
  if (usage.input || usage.output) add(4, TokenSummary(usage));
  add(5, CacheSummary(usage));
  if (view.verbose) add(6, "verbose");
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
  // Drop the least valuable segment until the row fits, the rightmost of a
  // tie first; StatusBarLine still performs the final UTF-8-safe clipping.
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

// Transient work state for the pinned row while a turn runs. StatusBar owns
// the idle row; this one animates, so it is handed the frame clock instead of
// reading it — which is also what lets a test drive a frame without a turn.
struct ActivityView {
  std::chrono::steady_clock::duration elapsed{};
  int64_t context_used = 0;
  int64_t context_window = 0;
  // The active route in schema form, the same string StatusView::model
  // spells, so both rows name the route identically.
  std::string model{};
  size_t background = 0;
  size_t foreground = 0;
  // Delegated children, counted apart from `background` so the row can say
  // what the other processes are rather than lumping them into one number.
  size_t subagents = 0;
  size_t queued = 0;
  bool interrupting = false;
  // The newest child's progress, "<id>: <line>". Terminal output from another
  // process: rendered through TerminalSafe, never raw.
  std::string subagent{};
};

// The working row: spinner frame, activity label, and the same "drop what does
// not fit" discipline as StatusBar, except here the label is what gets cut
// because the counters on the right are the part that changes.
inline std::string ActivityBar(const ActivityView& view) {
  std::string seconds =
      FmtDuration(std::chrono::duration<double>(view.elapsed).count());
  std::string activity = CurrentTerminalActivity();
  static constexpr auto kSpinnerInterval = std::chrono::milliseconds(100);
  auto ticks =
      std::chrono::duration_cast<std::chrono::milliseconds>(view.elapsed) /
      kSpinnerInterval;
  std::string prefix = SpinnerFrame(static_cast<size_t>(ticks));
  prefix += " ";
  // What the turn is doing, in descending order of how directly the human
  // asked for it. A model round is the one label that names no work, so a
  // delegated child's progress takes those columns and yields again as soon
  // as this process has something of its own to report.
  const bool waiting =
      activity.empty() || activity.starts_with(kWaitingActivity);
  std::string state =
      activity.empty() ? std::string(kWaitingActivity) : activity;
  if (view.interrupting) {
    state = "Interrupting";
  } else if (waiting && !view.subagent.empty()) {
    // Terminal output from another process. It is not sanitized here because
    // ActivityLabel already does it for every label this row can carry --
    // duplicating that would leave two places to keep honest instead of one.
    state = view.subagent;
  }
  auto counted = [](const char* label, size_t count) {
    return count ? label + FmtCount(static_cast<int64_t>(count)) : "";
  };
  std::string suffix =
      " · " + JoinDot({view.model, seconds,
                       ContextSummary(view.context_used, view.context_window),
                       counted("agents:", view.subagents),
                       counted("bg:", view.background),
                       counted("steer:", view.queued)});
  // The keys that act on a running turn: the lowest priority, so the hint is
  // the first thing a narrow terminal gives up.
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
  size_t activity_width = width > reserved ? width - reserved : 0;
  return prefix + ActivityLabel(state, activity_width) + suffix;
}

// The pinned status row: dim, clipped to the terminal, and cleared to the
// right so a shorter line never leaves stale text behind.
inline std::string StatusBarLine(const std::string& status) {
  std::string text =
      DisplayTrunc(AsciiGlyphs(TerminalSafe(status)), TerminalWidth(1));
  return std::string(RST()) + DIM() + text + "\033[K" + RST();
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_DISPLAY_H_
