// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_DISPLAY_H_
#define UAGENT_INCLUDE_UI_DISPLAY_H_
// Terminal rendering for the REPL: model/route listings and the status line.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "include/api.h"
#include "include/core/env.h"
#include "include/core/strings.h"
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
  std::string context = "ctx " + FmtCount(used);
  if (window > 0) context += "/" + FmtCount(window);
  return context;
}

// Compact session metadata for the persistent composer. Keep the stable
// identity first; transient work state gets its own line while a turn runs.
struct StatusView {
  int64_t context_used = 0;
  // The active route in schema form, [provider/]model[:variant][:effort], so
  // the row shows a selection the user could paste back into --model.
  std::string model;
  // Only set when no provider scope was resolvable, where the bare model id
  // alone would not say where the request goes.
  std::string host;
  bool verbose = false;
  bool yolo = false;
  size_t attachments = 0;
  size_t background = 0;
};

// One ordered list of segments, rendered in place and dropped by priority when
// the terminal is too narrow. A second hand-maintained "cramped" spelling of
// the same row is how the two copies used to drift.
inline std::string StatusBar(const Api& api, const Usage& usage,
                             const StatusView& view) {
  struct Segment {
    int priority;  // higher is dropped first
    std::string text;
  };
  std::vector<Segment> segments;
  auto add = [&segments](int priority, std::string text) {
    if (!text.empty()) segments.push_back({priority, std::move(text)});
  };

  add(0, view.host.empty() ? view.model : view.model + " @ " + view.host);
  add(1, ContextSummary(view.context_used, api.ctx_window));
  add(3, ContextLeftSummary(view.context_used, api.ctx_window));
  if (usage.input || usage.output) add(4, TokenSummary(usage));
  add(5, CacheSummary(usage));
  if (usage.cost > 0) add(2, FmtCost(usage.cost));
  if (view.background) {
    add(3, "bg:" + FmtCount(static_cast<int64_t>(view.background)));
  }
  if (view.attachments) {
    add(3, FmtCount(static_cast<int64_t>(view.attachments)) + " attached");
  }
  if (view.verbose) add(6, "verbose");
  add(7, "/help for shortcuts");
  add(2, view.yolo ? "YOLO" : std::string());

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
  // Drop the least valuable segment until the row fits; PrintStatusBar still
  // performs the final UTF-8-safe clipping.
  while (DisplayWidth(line) > TerminalWidth(1) && segments.size() > 1) {
    auto victim = std::max_element(segments.begin(), segments.end(),
                                   [](const Segment& a, const Segment& b) {
                                     return a.priority < b.priority;
                                   });
    if (victim->priority == 0) break;
    segments.erase(victim);
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
  std::string model;
  size_t background = 0;
  size_t foreground = 0;
  // Delegated children, counted apart from `background` so the row can say
  // what the other processes are rather than lumping them into one number.
  size_t subagents = 0;
  size_t queued = 0;
  bool interrupting = false;
  // The newest child's progress, "<id>: <line>". Terminal output from another
  // process: rendered through TerminalSafe, never raw.
  std::string subagent;
};

// The working row: spinner frame, activity label, and the same "drop what does
// not fit" discipline as StatusBar, except here the label is what gets cut
// because the counters on the right are the part that changes.
inline std::string ActivityBar(const ActivityView& view) {
  std::string seconds =
      FmtDuration(std::chrono::duration<double>(view.elapsed).count());
  std::string activity = CurrentTerminalActivity();
  static constexpr auto kSpinnerInterval = std::chrono::milliseconds(100);
  static constexpr const char* kFrames[] = {"⠋", "⠙", "⠹", "⠸", "⠼",
                                            "⠴", "⠦", "⠧", "⠇", "⠏"};
  auto ticks =
      std::chrono::duration_cast<std::chrono::milliseconds>(view.elapsed) /
      kSpinnerInterval;
  std::string prefix = kFrames[static_cast<size_t>(ticks) % 10];
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
  std::string route = view.model.empty() ? std::string() : " · " + view.model;
  std::string suffix = " · " + seconds;
  suffix += " · " + ContextSummary(view.context_used, view.context_window);
  if (view.subagents > 0) {
    suffix += " · agents:" + FmtCount(static_cast<int64_t>(view.subagents));
  }
  if (view.background > 0) {
    suffix += " · bg:" + FmtCount(static_cast<int64_t>(view.background));
  }
  if (view.foreground > 0) {
    suffix += " · Ctrl+B background";
    if (view.foreground > 1) {
      suffix +=
          " " + FmtCount(static_cast<int64_t>(view.foreground)) + " commands";
    }
  }
  if (view.queued > 0) {
    suffix += " · steer:" + FmtCount(static_cast<int64_t>(view.queued));
  }
  size_t width = TerminalWidth(1);
  // The route is the part of this row that yields when a rolling ticker wants
  // the same columns: it never changes during a turn and the idle row names it
  // anyway, whereas a window under roughly forty columns shows fragments of
  // words rather than a readable phrase — and a fully qualified route id can
  // take half a narrow terminal by itself.
  if (!route.empty() && CurrentTerminalActivityRolling()) {
    static constexpr size_t kReadableTicker = 32;
    size_t taken =
        DisplayWidth(prefix) + DisplayWidth(suffix) + DisplayWidth(route);
    if (width < taken + kReadableTicker) route.clear();
  }
  suffix = route + suffix;
  if (SteeringEnabled()) {
    std::string hint = " · Esc to interrupt";
    // A rolling ticker always holds more text than fits, so it asks for
    // the full cap instead of the width of its idle fallback label.
    size_t desired = std::min<size_t>(
        CurrentTerminalActivityRolling() ? 64 : DisplayWidth(state), 64);
    size_t with_hint = DisplayWidth(prefix) + DisplayWidth(suffix) +
                       DisplayWidth(hint) + desired;
    if (with_hint <= width) suffix += hint;
  }
  size_t reserved = DisplayWidth(prefix) + DisplayWidth(suffix);
  size_t activity_width = width > reserved ? width - reserved : 0;
  if (CurrentTerminalActivityRolling()) {
    // Rolling ticker: render a sliding window of the reasoning instead of
    // the static fallback label so it animates with the status frame.
    // ActivityLabel is a no-op while the window fits; on a terminal too
    // narrow even for the ticker label it bounds the row as usual.
    return prefix +
           ActivityLabel(RenderCurrentTerminalActivity(activity_width),
                         activity_width) +
           suffix;
  }
  return prefix + ActivityLabel(state, activity_width) + suffix;
}

// The pinned status row: dim, clipped to the terminal, and cleared to the
// right so a shorter line never leaves stale text behind.
// Continuation rows a status row of `columns` display columns has been
// rewrapped into by a terminal now `width` columns wide. Zero unless the
// terminal has narrowed since the row was written.
inline size_t StatusOverflowRows(size_t columns, size_t width) {
  return columns > 0 && width > 0 ? (columns - 1) / width : 0;
}

// `columns` reports the width the row actually occupies, which the caller
// needs to erase it again after a terminal that rewraps has resized.
inline std::string StatusBarLine(const std::string& status,
                                 size_t* columns = nullptr) {
  std::string text =
      DisplayTrunc(AsciiGlyphs(TerminalSafe(status)), TerminalWidth(1));
  if (columns) *columns = DisplayWidth(text);
  return std::string(RST()) + DIM() + text + "\033[K" + RST();
}

inline void PrintStatusBar(const std::string& status) {
  if (!g_tty) {
    printf("%s\n", AsciiGlyphs(TerminalSafe(status)).c_str());
    return;
  }
  printf("%s\n", StatusBarLine(status).c_str());
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_DISPLAY_H_
