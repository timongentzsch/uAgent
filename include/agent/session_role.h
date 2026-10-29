// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_AGENT_SESSION_ROLE_H_
#define UAGENT_INCLUDE_AGENT_SESSION_ROLE_H_
// The role a session can have beyond an ordinary conversation: an agent
// another conversation started, whose header's `agent` says by whom and how.

#include "include/core/json.h"

namespace uagent {
// A decision with this route is the agent's parent's to answer, and a
// command with this origin is the parent's: its brief or its decision.
inline constexpr const char* kRouteParent = "parent";

// An agent's role from its options or header: {parent (its session id),
// edits (none, worktree or folder), depth, budget, streak}. Empty for any
// other session.
inline json AgentOf(const json& session) {
  return JsonValue(session, "agent", json::object());
}

// A pending decision or approval the agent's parent decides first.
inline bool RoutedToParent(const json& pending) {
  return JsonValue(pending, "route", "") == kRouteParent;
}

// A pending decision or approval that waits on a person rather than on the
// agent's parent.
inline bool WaitsOnPerson(const json& pending) {
  return pending.is_object() && !RoutedToParent(pending);
}

// How a session stands to someone looking from outside, from what its
// runtime last said: a decision still with the parent counts as working.
inline const char* Standing(const json& pending, bool busy) {
  return WaitsOnPerson(pending) ? "needs you" : busy ? "working" : "idle";
}
}  // namespace uagent
#endif
