// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_CHILD_AGENT_H_
#define UAGENT_INCLUDE_AGENT_CHILD_AGENT_H_
// The headless child a session runs for memory extraction, and the session
// file this process saves to.

#include <string>

#include "include/core/child_env.h"
#include "include/providers/providers.h"

namespace uagent {

// The last override of a name wins, so callers append their policy after this.
EnvironmentOverrides ChildAgentEnvironment(Route route,
                                           const std::string& session);

// From UAGENT_INTERNAL_SESSION_PATH; empty for a run that never saves.
std::string OwnSessionFile();
// Plan mode's plan, beside that file; empty with it.
std::string PlanFile();

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_CHILD_AGENT_H_
