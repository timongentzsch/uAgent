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

// Only an explicitly non-UTF-8 locale downgrades the glyphs. An unset locale
// is ordinary on capable terminals, so it is not evidence of the opposite.
bool ResolveUnicodeEnabled() {
  for (const char* name : {"LC_ALL", "LC_CTYPE", "LANG"}) {
    const std::string value = AsciiLower(EnvStr(name));
    if (value.empty()) continue;
    return value.find("utf-8") != std::string::npos ||
           value.find("utf8") != std::string::npos;
  }
  return true;
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
  };
  std::vector<Entry> active;
};

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

}  // namespace uagent
