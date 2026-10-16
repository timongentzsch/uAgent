// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_AGENT_SESSION_ROLE_H_
#define UAGENT_INCLUDE_AGENT_SESSION_ROLE_H_
// The roles a session can have beyond an ordinary conversation: a folder's
// panel, or a thread it started. A session header's `kind` names it.

#include <algorithm>
#include <iterator>
#include <string_view>

#include "include/core/json.h"

namespace uagent {
inline constexpr const char* kSessionKindPanel = "panel";
inline constexpr const char* kSessionKindThread = "thread";
// A thread's decision with this route is its panel's to answer.
inline constexpr const char* kRoutePanel = "panel";

// Whether a session's options or header name it a thread of a panel.
inline bool IsThread(const json& session) {
  return JsonValue(session, "kind", "") == kSessionKindThread;
}

// A thread's link to its panel (its folder, ceiling, chat membership) from a
// session's options or header; empty for any other session.
inline json ThreadRole(const json& session) {
  return JsonValue(session, "thread", json::object());
}

// Who a thread is in its panel's chat (name, persona, skills), from its
// `thread` role; empty for a thread that works a brief.
inline json ChatMember(const json& thread) {
  return JsonValue(thread, "member", json::object());
}

// A chat participant's whole answer when it has nothing to say now: it
// passes, or waits to read someone who is typing. Shown to nobody.
inline constexpr std::string_view kSilentAnswers[] = {"PASS", "PASS.", "WAIT",
                                                      "WAIT."};
inline bool SilentAnswer(std::string_view text) {
  return std::ranges::find(kSilentAnswers, text) != std::end(kSilentAnswers);
}

// A pending decision or approval the thread's panel decides first.
inline bool RoutedToPanel(const json& pending) {
  return JsonValue(pending, "route", "") == kRoutePanel;
}

// A pending decision or approval that waits on a person rather than on the
// thread's panel.
inline bool WaitsOnPerson(const json& pending) {
  return pending.is_object() && !RoutedToPanel(pending);
}

// How a session stands to someone looking from outside, from what its
// runtime last said: a decision still with the panel counts as working.
inline const char* Standing(const json& pending, bool busy) {
  return WaitsOnPerson(pending) ? "needs you" : busy ? "working" : "idle";
}
}  // namespace uagent
#endif
