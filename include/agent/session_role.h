// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_AGENT_SESSION_ROLE_H_
#define UAGENT_INCLUDE_AGENT_SESSION_ROLE_H_
// The roles a session can have beyond an ordinary conversation: a folder's
// coordinator, or a thread it started. A session header's `kind` names it.

#include "include/core/json.h"

namespace uagent {
inline constexpr const char* kSessionKindCoordinator = "coordinator";
inline constexpr const char* kSessionKindThread = "thread";
// A thread's decision with this route is its coordinator's to answer.
inline constexpr const char* kRouteCoordinator = "coordinator";

// Who a thread is in its coordinator's chat (name, persona, skills), from its
// `thread` role; empty for a thread that works a brief.
inline json ChatMember(const json& thread) {
  return JsonValue(thread, "member", json::object());
}

// A pending decision or approval that waits on a person rather than on the
// thread's coordinator.
inline bool WaitsOnPerson(const json& pending) {
  return pending.is_object() &&
         JsonValue(pending, "route", "") != kRouteCoordinator;
}

// How a session stands to someone looking from outside, from what its
// runtime last said: a decision still with the coordinator counts as working.
inline const char* Standing(const json& pending, bool busy) {
  return WaitsOnPerson(pending) ? "needs you" : busy ? "working" : "idle";
}
}  // namespace uagent
#endif
