// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_UAGENT_TOOL_H_
#define UAGENT_INCLUDE_APP_UAGENT_TOOL_H_
// The uagent tool: the model inspects this build and proposes (never
// applies) a change to its settings or to a file that shapes it, or a slash
// command to run. And the schedule tool, which manages scheduled tasks.

#include <functional>
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

// Takes a slash command line a person approved; the application runs it as
// the turn ends.
using CommandQueue = std::function<void(const std::string&)>;

// One tool, by `action`. show inspects and is offered everywhere. set (the
// settings) and write (an instructions file, the stylesheet, the system
// message's changes, the folder's .mcp.json, a skill) prepare and preview;
// the mandatory-human approval lane decides, and only what was approved is
// written. There is deliberately no commit argument the model can set.
// Without `prepare`, where nobody can approve or the policy forbids changes,
// the tool only shows and its schema offers nothing else. With `queue`, in a
// conversation's own session, command hands it one slash command line, which
// a person approved as that line.
Tool UagentTool(SelfDescriptionProvider describe,
                ConfigProposalFactory prepare = nullptr,
                CommandQueue queue = nullptr);

// Scheduled tasks for this folder, approved like any other change: a task
// runs on `model` (the conversation's, read at each call) with no more
// approval reach than the conversation that saved it.
Tool ScheduleTool(std::function<std::string()> model);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_UAGENT_TOOL_H_
