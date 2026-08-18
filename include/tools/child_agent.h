// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_CHILD_AGENT_H_
#define UAGENT_INCLUDE_TOOLS_CHILD_AGENT_H_
// What every delegated child shares: the route it runs on, the ledger it
// reports usage to, and how it is invoked. The subagent, advisor and memory
// extractor differ only in the policy they append — toolset, limits,
// memory — so keeping the common half here is what stops one of them from
// quietly missing a field the others gained.

#include <optional>
#include <string>

#include "include/api.h"
#include "include/core/child_env.h"
#include "include/providers.h"
#include "include/tools/process.h"
#include "include/tools/tool.h"

namespace uagent {

// Route, depth and usage ledger. Overrides are applied in order with the last
// occurrence winning, so callers append their own policy after this.
EnvironmentOverrides ChildAgentEnvironment(SideRoute route);

// The child is always headless and self-approving; it has only the tools its
// toolset grants it.
std::string ChildAgentCommand(bool debug, const std::string& prompt);

// Under a session budget children run one at a time: two concurrent ones would
// each be told the whole remainder and could overshoot together. Returns the
// refusal to hand back, or nothing when the call may proceed, and reports the
// budget the child should inherit.
std::optional<ToolResult> ChildAgentBudgetBlock(
    const Api& api, const ProcessSupervisor& processes, double& remaining);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_CHILD_AGENT_H_
