// Copyright 2026 Timon Gentzsch

#include "include/core/term.h"

#include <algorithm>
#include <atomic>
#include <clocale>
#include <mutex>
#include <string>
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

// A dumb terminal shows no SGR at all; any other one shows attributes.
bool ResolveAttributesEnabled(bool tty) {
  return tty && EnvStr("TERM") != "dumb";
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

// Plain drops everything a screen reader would speak as noise; the terminal
// is then written like a pipe, except that colour still follows NO_COLOR.
void ApplyTerminalProfile(bool plain, bool reduced_motion) {
  g_plain = plain;
  g_motion = !plain && !reduced_motion;
  if (!plain) return;
  g_tty = false;
  g_signal_tty = 0;
  g_unicode = false;
}

bool EnsureUtf8Ctype() {
  if (MB_CUR_MAX > 1) return true;
  for (const char* name : {"C.UTF-8", "en_US.UTF-8"}) {
    if (std::setlocale(LC_CTYPE, name)) return true;
  }
  return false;
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

}  // namespace

uint64_t BeginTerminalActivity(std::string label) {
  TerminalActivityState& state = TerminalActivities();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.active.push_back({++state.next, std::move(label)});
  return state.next;
}

void UpdateTerminalActivity(uint64_t id, std::string label) {
  TerminalActivityState& state = TerminalActivities();
  std::lock_guard<std::mutex> lock(state.mutex);
  for (TerminalActivityState::Entry& entry : state.active) {
    if (entry.id == id) entry.label = label;
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

}  // namespace uagent
