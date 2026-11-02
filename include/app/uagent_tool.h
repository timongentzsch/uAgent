// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_UAGENT_TOOL_H_
#define UAGENT_INCLUDE_APP_UAGENT_TOOL_H_
// The uagent tools: the model inspects this build, proposes (never applies)
// a change to its settings or instructions, and manages scheduled tasks.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "include/app/config_proposal.h"
#include "include/app/self_description.h"
#include "include/tools/tool.h"

namespace uagent {

// Builds a proposal from live state. Supplied by the application so the tool
// never holds a stale snapshot of the configuration layers.
using ConfigProposalFactory = std::function<ConfigProposal(
    ConfigProposalScope, const std::vector<ConfigChange>&)>;

using SelfDescriptionProvider =
    std::function<json(SelfTopic, const std::string&)>;
using ConfigApprovals = ApprovedProposals<ConfigProposal>;

// Inspects this build; offered everywhere.
Tool UagentTool(SelfDescriptionProvider describe);

// The two writing tools, offered only where a person can approve. Each
// prepares and previews; the mandatory-human approval lane decides, and only
// what was approved is written. There is deliberately no commit argument the
// model can set.
Tool UagentConfigureTool(const ConfigProposalFactory& prepare,
                         const std::shared_ptr<ConfigApprovals>& store);
Tool UagentInstructionsTool();

// Scheduled tasks for this folder, approved like any other change: a task
// runs on `model` (the conversation's, read at each call) with no more
// approval reach than the conversation that saved it.
Tool ScheduleTool(std::function<std::string()> model);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_UAGENT_TOOL_H_
