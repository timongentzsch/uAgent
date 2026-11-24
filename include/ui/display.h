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

// Earlier segments win ties. The rightmost lowest-priority segment drops
// first, leaving the text in reading order however much room the row has.
struct StatusSegment {
  int priority;
  std::string text;
};

inline std::string FitStatus(std::vector<StatusSegment> segments) {
  std::erase_if(segments,
                [](const auto& segment) { return segment.text.empty(); });
  if (g_tty && !segments.empty()) {
    const size_t width = TerminalWidth(1);
    size_t used = 3 * (segments.size() - 1);  // " · " between segments
    for (const auto& segment : segments) used += DisplayWidth(segment.text);
    while (used > width && segments.size() > 1) {
      auto victim =
          std::max_element(segments.rbegin(), segments.rend(),
                           [](const StatusSegment& a, const StatusSegment& b) {
                             return a.priority < b.priority;
                           });
      used -= 3 + DisplayWidth(victim->text);
      segments.erase(std::next(victim).base());
    }
  }
  std::string line;
  for (const auto& segment : segments) {
    if (!line.empty()) line += " · ";
    line += segment.text;
  }
  return line;
}

inline std::string ApprovalLabel(const std::string& mode) {
  return mode == "yolo"   ? "YOLO"
         : mode == "auto" ? "Auto"
         : mode == "plan" ? "Plan"
                          : "Ask";
}

inline std::string StatusBar(const Usage& usage, const StatusView& view) {
  std::vector<StatusSegment> segments;
  auto add = [&segments](int priority, std::string text) {
    segments.push_back({priority, std::move(text)});
  };
  add(0, view.turn_status);
  add(1, view.model);
  add(0, ApprovalLabel(view.approval));
  add(2, ContextLeftSummary(view.context_used, view.context_window));
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

  return FitStatus(std::move(segments));
}

// Work state for the pinned row while a turn runs. It is handed the frame
// clock instead of reading it, so a test can drive a frame.
struct TurnStatusView {
  std::chrono::steady_clock::duration elapsed{};
  int64_t context_used = 0;
  int64_t context_window = 0;
  // The active route, spelled as StatusView::model.
  std::string model{};
  std::string approval;
  size_t background = 0;
  size_t foreground = 0;
  size_t queued = 0;
  bool interrupting = false;
};

// Reserve room for the action, effective mode and interrupt key before metrics.
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
  const std::string mode = ApprovalLabel(view.approval);
  const size_t width = TerminalWidth(1);
  const size_t reserve = DisplayWidth(prefix + " · " + mode + " · Esc stop");
  const size_t room = width > reserve ? width - reserve : 6;
  std::string background_hint = "Ctrl+B background";
  if (view.foreground > 1) {
    background_hint +=
        " " + FmtCount(static_cast<int64_t>(view.foreground)) + " commands";
  }
  return FitStatus({
      {0, prefix + StatusLabel(state, std::min<size_t>(64, room))},
      {1, mode},
      {5, view.model},
      {3, seconds},
      {6, ContextSummary(view.context_used, view.context_window)},
      {4, counted("act:", view.background)},
      {4, counted("steer:", view.queued)},
      {1, "Esc stop"},
      {2, background_hint},
  });
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
