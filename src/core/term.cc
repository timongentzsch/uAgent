// Copyright 2026 Timon Gentzsch

#include "include/core/term.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "include/core/strings.h"

namespace uagent {

namespace {
std::atomic<bool>& PersistentComposerFlag() {
  static std::atomic<bool> active{false};
  return active;
}
}  // namespace

void SetPersistentComposer(bool active) { PersistentComposerFlag() = active; }

bool PersistentComposer() { return PersistentComposerFlag(); }

namespace {

struct TerminalActivityState {
  std::mutex mutex;
  uint64_t next = 0;
  struct Entry {
    uint64_t id = 0;
    std::string label;        // static fallback label
    std::string roll_prefix;  // caller-owned label kept ahead of the window
    std::string roll;         // bounded rolling ticker text
    double roll_cursor = 0;   // fractional display-column cursor
    double roll_edge = 0;     // last frame's live edge, to measure arrival
    std::chrono::steady_clock::time_point roll_last{};  // last advance tick
    bool rolling = false;
  };
  std::vector<Entry> active;
};

TerminalActivityState& TerminalActivities() {
  static TerminalActivityState state;
  return state;
}

}  // namespace

uint64_t BeginTerminalActivity(std::string label) {
  TerminalActivityState& state = TerminalActivities();
  std::lock_guard<std::mutex> lock(state.mutex);
  uint64_t id = ++state.next;
  TerminalActivityState::Entry entry;
  entry.id = id;
  entry.label = std::move(label);
  state.active.push_back(std::move(entry));
  return id;
}

void UpdateTerminalActivity(uint64_t id, std::string label) {
  TerminalActivityState& state = TerminalActivities();
  std::lock_guard<std::mutex> lock(state.mutex);
  for (auto& entry : state.active) {
    if (entry.id == id) {
      entry.label = std::move(label);
      entry.rolling = false;
      entry.roll_prefix.clear();
      entry.roll.clear();
      entry.roll_cursor = 0;
      return;
    }
  }
}

// Put the activity into rolling-ticker mode with the current reasoning buffer.
// The first transition joins the stream at its live edge (a cursor past the
// end is clamped to the newest text on the next frame); afterwards the buffer
// grows with each streamed delta and the window scrolls to follow it.
void SetTerminalActivityRolling(uint64_t id, const std::string& prefix,
                                const std::string& text) {
  TerminalActivityState& state = TerminalActivities();
  std::lock_guard<std::mutex> lock(state.mutex);
  for (auto& entry : state.active) {
    if (entry.id == id) {
      if (!entry.rolling) {
        entry.roll_cursor = static_cast<double>(DisplayWidth(text));
        entry.roll_edge = entry.roll_cursor;
        entry.roll_last = std::chrono::steady_clock::now();
      }
      entry.roll_prefix = prefix;
      entry.roll = text;
      entry.rolling = true;
      return;
    }
  }
}

void EndTerminalActivity(uint64_t id) {
  TerminalActivityState& state = TerminalActivities();
  std::lock_guard<std::mutex> lock(state.mutex);
  std::erase_if(state.active, [id](const TerminalActivityState::Entry& entry) {
    return entry.id == id;
  });
}

std::string CurrentTerminalActivity() {
  TerminalActivityState& state = TerminalActivities();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.active.empty() ? std::string() : state.active.back().label;
}

bool CurrentTerminalActivityRolling() {
  TerminalActivityState& state = TerminalActivities();
  std::lock_guard<std::mutex> lock(state.mutex);
  return !state.active.empty() && state.active.back().rolling;
}

// Render the most recent activity for one animation frame. In rolling mode the
// window slides left at a fixed readable rate (never faster than the stream
// delivers it), so reasoning can be read as it scrolls past; otherwise the
// static label is returned. Advancement is time-based and accumulates a
// fractional cursor, so a burst of render calls never outruns the pace.
std::string RenderCurrentTerminalActivity(size_t columns) {
  TerminalActivityState& state = TerminalActivities();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.active.empty()) return "";
  TerminalActivityState::Entry& entry = state.active.back();
  if (!entry.rolling || entry.roll.empty()) return entry.label;
  size_t reserved = DisplayWidth(entry.roll_prefix);
  if (reserved >= columns) return entry.roll_prefix;
  columns -= reserved;

  // The window tracks the newest text: reasoning arrives far faster than any
  // readable scroll rate, so a fixed rate would fall behind without bound and
  // show text that is already stale. The cursor instead chases the live edge,
  // moving at least kRollColsPerSec and always closing the remaining gap
  // within kCatchUpSeconds, which bounds staleness in time rather than in
  // columns — the lag settles at kCatchUpSeconds of text at any arrival rate.
  //
  // That leaves one honest limit: when the stream outruns the window, no motion
  // is readable — sliding by that much is not scrolling but a flicker of
  // fragments landing mid-word. The choice is made from how fast the live edge
  // is moving, not from the gap that has accumulated: deciding per gap makes
  // the ticker drift and then snap by whole windows, which is worse than
  // either mode. Above half a window of new text per frame it simply follows
  // the edge, the way a status row should when the text will not fit through
  // it; below that it slides.
  constexpr double kRollColsPerSec = 14.0;
  constexpr double kCatchUpSeconds = 0.5;
  constexpr double kReadableFraction = 0.5;
  auto now = std::chrono::steady_clock::now();
  double secs = std::chrono::duration<double>(now - entry.roll_last).count();
  entry.roll_last = now;

  size_t total = DisplayWidth(entry.roll);
  size_t cols = columns > total ? total : columns;
  double target = static_cast<double>(total > cols ? total - cols : 0);
  double arrived = target - entry.roll_edge;
  entry.roll_edge = target;
  double gap = target - entry.roll_cursor;
  if (arrived > static_cast<double>(cols) * kReadableFraction) {
    entry.roll_cursor = target;
  } else if (gap > 0) {
    double step = std::max(kRollColsPerSec, gap / kCatchUpSeconds) * secs;
    entry.roll_cursor = std::min(target, entry.roll_cursor + step);
  } else {
    // Past the live edge: the first frame, or a buffer trimmed at the front.
    entry.roll_cursor = target;
  }
  return entry.roll_prefix +
         DisplayWindow(entry.roll, static_cast<size_t>(entry.roll_cursor),
                       cols);
}

}  // namespace uagent
