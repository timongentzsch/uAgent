// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_AGENT_SESSION_ROLE_H_
#define UAGENT_INCLUDE_AGENT_SESSION_ROLE_H_
// The role a session can have beyond an ordinary conversation: an agent
// another conversation started, whose header's `agent` says by whom and how.

#include "include/core/json.h"

namespace uagent {
// The route of a decision, and the origin of a command, that is the parent's.
inline constexpr const char* kRouteParent = "parent";

// {parent (session id), edits (none|worktree|folder), depth, budget, streak}.
inline json AgentOf(const json& session) {
  return JsonValue(session, "agent", json::object());
}

inline bool RoutedToParent(const json& pending) {
  return JsonValue(pending, "route", "") == kRouteParent;
}

inline bool WaitsOnPerson(const json& pending) {
  return pending.is_object() && !RoutedToParent(pending);
}

// A decision still with the parent counts as working.
inline const char* Standing(const json& pending, bool busy) {
  return WaitsOnPerson(pending) ? "needs you" : busy ? "working" : "idle";
}
}  // namespace uagent
#endif
