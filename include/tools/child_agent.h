// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_CHILD_AGENT_H_
#define UAGENT_INCLUDE_TOOLS_CHILD_AGENT_H_
// What every delegated child shares: the route it runs on, the ledger it
// reports usage to, and how it is invoked. The subagent and memory extractor
// differ only in the policy they append — toolset, limits,
// memory — so keeping the common half here is what stops one of them from
// quietly missing a field the others gained.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "include/api.h"
#include "include/core/child_env.h"
#include "include/providers.h"
#include "include/tools/process.h"
#include "include/tools/tool.h"

namespace uagent {

enum class ChildAgentFailureStage {
  kRouteResolution,
  kSpawn,
  kExecution,
};

// A bounded, route-explicit failure record shared by synchronous delegation
// and background completion. It records that policy was preserved rather than
// hiding a provider/model fallback behind a retry.
std::string ChildAgentFailureReport(std::string_view route,
                                    ChildAgentFailureStage stage,
                                    std::string_view diagnostics = {});

// Route, depth and usage ledger. Overrides are applied in order with the last
// occurrence winning, so callers append their own policy after this.
EnvironmentOverrides ChildAgentEnvironment(SideRoute route);

// The child is always headless and self-approving; it has only the tools its
// toolset grants it. It reports in the headless JSON envelope, so the parent
// reads an answer and a stop reason rather than guessing from prose.
std::string ChildAgentCommand(bool debug, const std::string& prompt);

// The child's answer, followed by what the caller has to know to decide: any
// ceiling that was clamped on the way in, and the limit that ended the child
// if one did. Raw output is returned verbatim when no envelope is found,
// labelled as raw rather than passed off as an answer.
std::string ChildAgentAnswer(std::string output,
                             const std::vector<std::string>& clamped);
std::string ChildAgentConstraintNotes(const std::vector<std::string>& clamped);
std::optional<json> ChildAgentEnvelope(const std::string& output);
// A tool result is read through a cap sized for what a reader can absorb, and
// a tail that begins inside the child's record leaves half a JSON object,
// which parses nowhere: a child that answered is then reported as having
// produced nothing at all. When the capped text no longer carries the record,
// it is recovered whole from the retained log.
std::string ChildAgentRecoverEnvelope(std::string output,
                                      const std::string& log_path);

std::string ChildAgentStopNote(const json& stop);

// The collaborator record this process is resuming, or empty when this process
// is not a collaborator. The parent hands it down as a path in the child's
// environment, so it is validated once here -- inside the collaborators
// directory, canonicalized -- rather than trusted at each of the places that
// ask. Memoized: it is fixed for the lifetime of the process, and it answers
// the question "am I somebody's child" on paths that run every step.
const std::string& CollaboratorSessionFile();

// Under a session budget children run one at a time: two concurrent ones would
// each be told the whole remainder and could overshoot together. Returns the
// refusal to hand back, or nothing when the call may proceed, and reports the
// cost and generated-token budgets the child should inherit.
std::optional<ToolResult> ChildAgentBudgetBlock(
    const Api& api, const ProcessSupervisor& processes, double& remaining_cost,
    int64_t& remaining_tokens);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_CHILD_AGENT_H_
