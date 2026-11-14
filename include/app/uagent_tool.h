// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_UAGENT_TOOL_H_
#define UAGENT_INCLUDE_APP_UAGENT_TOOL_H_
// The uagent tool, by which the model inspects this build and proposes
// changes to it, and the schedule tool for scheduled tasks.

#include <functional>
#include <string>
#include <vector>

#include "include/app/config_proposal.h"
#include "include/app/self_description.h"
#include "include/tools/tool.h"

namespace uagent {

// Builds a proposal from live state, never from a stale snapshot.
using ConfigProposalFactory = std::function<ConfigProposal(
    ConfigProposalScope, const std::vector<ConfigChange>&)>;

using SelfDescriptionProvider =
    std::function<json(SelfTopic, const std::string&)>;

// Takes an approved slash command line, which runs as the turn ends.
using CommandQueue = std::function<void(const std::string&)>;

// show inspects; set and write only prepare and preview, and the human
// approval lane decides: there is no commit argument the model can set.
// Without `prepare` the tool only shows; `queue` takes an approved command.
Tool UagentTool(SelfDescriptionProvider describe,
                ConfigProposalFactory prepare = nullptr,
                CommandQueue queue = nullptr);

// Scheduled tasks for this folder. A task runs on `model` (read at each
// call) with no more approval reach than the conversation that saved it.
Tool ScheduleTool(std::function<std::string()> model);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_UAGENT_TOOL_H_
