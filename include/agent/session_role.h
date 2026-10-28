// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_AGENT_SESSION_ROLE_H_
#define UAGENT_INCLUDE_AGENT_SESSION_ROLE_H_
// The roles a session can have beyond an ordinary conversation: a folder's
// panel, or a thread it started. A session header's `kind` names it.

#include "include/core/json.h"

namespace uagent {
inline constexpr const char* kSessionKindPanel = "panel";
inline constexpr const char* kSessionKindThread = "thread";
// A thread's decision with this route is its panel's to answer.
inline constexpr const char* kRoutePanel = "panel";
// The origin of a thread's first message, its panel's brief: a turn the
// panel starts, though not one of the messages its streak counts.
inline constexpr const char* kOriginBrief = "brief";

// Whether a session's options or header name it a thread of a panel.
inline bool IsThread(const json& session) {
  return JsonValue(session, "kind", "") == kSessionKindThread;
}

// The role a delegating session gave this one, from its options or header:
// {parent, parent_session, ...}. A subagent's lives for its parent's turn;
// a thread's has lifetime "thread" and adds its folder, day, ceiling, brief
// and step. Empty for any other session.
inline json DelegationOf(const json& session) {
  return JsonValue(session, "delegation", json::object());
}

// Whether `role` (DelegationOf) is a thread's: it outlives the turn that
// started it.
inline bool ThreadLifetime(const json& role) {
  return JsonValue(role, "lifetime", "") == kSessionKindThread;
}

// A thread's role from a session's options or header; empty for any other
// session.
inline json ThreadRole(const json& session) {
  json role = DelegationOf(session);
  return ThreadLifetime(role) ? role : json::object();
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
