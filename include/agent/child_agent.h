// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_CHILD_AGENT_H_
#define UAGENT_INCLUDE_AGENT_CHILD_AGENT_H_
// The headless child a session runs for its memory extraction: the route it
// runs on and the ledger it reports usage to. And the session file this
// process saves to.

#include <string>

#include "include/core/child_env.h"
#include "include/providers/providers.h"

namespace uagent {

// Route, depth and the usage ledger of `session`, the parent's id. Overrides
// are applied in order with the last occurrence winning, so callers append
// their own policy after this.
EnvironmentOverrides ChildAgentEnvironment(Route route,
                                           const std::string& session);

// The saved session file of this process (UAGENT_INTERNAL_SESSION_PATH,
// exported on first save), or empty for a run that never saves.
std::string OwnSessionFile();
// Plan mode's plan, beside that file; empty with it.
std::string PlanFile();

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_CHILD_AGENT_H_
