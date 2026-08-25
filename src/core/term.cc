// Copyright 2026 Timon Gentzsch

#include "include/core/term.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/strings.h"
#include "include/core/style.h"

namespace uagent {

// no-color.org and bixense.com/clicolors: an explicit opt-out wins over an
// explicit opt-in, and both win over the isatty answer.
bool ResolveColorEnabled(bool tty) {
  if (!EnvStr("NO_COLOR").empty()) return false;
  if (EnvStr("TERM") == "dumb") return false;
  const std::string force = EnvStr("CLICOLOR_FORCE");
  if (!force.empty() && force != "0") return true;
  return tty;
}

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
    std::string label;
    std::string roll_prefix;
    std::string roll;
    EdgeChaser chaser;
    bool rolling = false;
    // The buffer as it is drawn, plus the caller's normalizer. Recomputing it
    // only when the buffer changes keeps whole-buffer work off the token path.
    ActivityTextTransform roll_transform = nullptr;
    std::string roll_display;
    bool roll_stale = false;
  };
  std::vector<Entry> active;
};

const std::string& RollDisplay(TerminalActivityState::Entry& entry) {
  if (entry.roll_stale) {
    entry.roll_display =
        entry.roll_transform ? entry.roll_transform(entry.roll) : entry.roll;
    entry.roll_stale = false;
  }
  return entry.roll_display;
}

TerminalActivityState& TerminalActivities() {
  static TerminalActivityState state;
  return state;
}

// The live entry with this id, or nullptr. Callers hold state.mutex.
TerminalActivityState::Entry* FindActivityLocked(TerminalActivityState& state,
                                                 uint64_t id) {
  for (TerminalActivityState::Entry& entry : state.active) {
    if (entry.id == id) return &entry;
  }
  return nullptr;
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
  TerminalActivityState::Entry* entry = FindActivityLocked(state, id);
  if (!entry) return;
  entry->label = std::move(label);
  entry->rolling = false;
  entry->roll_prefix.clear();
  entry->roll.clear();
  entry->roll_display.clear();
  entry->roll_transform = nullptr;
  entry->roll_stale = false;
  entry->chaser = {};
}

// Put the activity into rolling-ticker mode with the current reasoning buffer.
// The first transition joins the stream at its live edge (a cursor past the
// end is clamped to the newest text on the next frame); afterwards the buffer
// grows with each streamed delta and the window scrolls to follow it.
void SetTerminalActivityRolling(uint64_t id, const std::string& prefix,
                                const std::string& text,
                                ActivityTextTransform transform) {
  TerminalActivityState& state = TerminalActivities();
  std::lock_guard<std::mutex> lock(state.mutex);
  TerminalActivityState::Entry* entry = FindActivityLocked(state, id);
  if (!entry) return;
  entry->roll_prefix = prefix;
  entry->roll = text;
  entry->roll_transform = transform;
  entry->roll_stale = true;
  if (!entry->rolling) {
    double w = static_cast<double>(DisplayWidth(RollDisplay(*entry)));
    entry->chaser.cursor = w;
    entry->chaser.edge = w;
    entry->chaser.last = std::chrono::steady_clock::now();
  }
  entry->rolling = true;
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
  if (!entry.rolling) return entry.label;
  const std::string& text = RollDisplay(entry);
  if (text.empty()) return entry.label;
  size_t reserved = DisplayWidth(entry.roll_prefix);
  if (reserved >= columns) return entry.roll_prefix;
  columns -= reserved;

  size_t total = DisplayWidth(text);
  size_t cols = columns > total ? total : columns;
  double target = static_cast<double>(total > cols ? total - cols : 0);
  entry.chaser.advance(target, cols, std::chrono::steady_clock::now());
  return entry.roll_prefix +
         DisplayWindow(text, static_cast<size_t>(entry.chaser.cursor), cols);
}

}  // namespace uagent
