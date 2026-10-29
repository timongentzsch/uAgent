// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_AGENT_TOOLS_H_
#define UAGENT_INCLUDE_APP_AGENT_TOOLS_H_
// The tools every agent has for the agents it starts: agent_start,
// agent_send, agents and decide. An agent is a conversation another one
// started, saved beside it; each runs in a session runtime of its own
// (session_worker.cc) and reports to its parent by mail (agent_link.h).

#include <functional>
#include <string>
#include <vector>

#include "include/api/client.h"
#include "include/tools/tool.h"

namespace uagent {
// Adds them to `tools`; agent_start only while this session may still start
// agents (agents.depth). `own_model` names the model this session is on
// right now, which an agent starts on unless told otherwise; `api` holds
// what is left of its budget.
void AddAgentTools(std::vector<Tool>& tools, const Api& api,
                   std::function<std::string()> own_model);

// Whether an agent of the session at `path` still owes it something: one
// working or not yet begun, or mail from one not yet taken or acknowledged.
// What a headless run waits for; an agent waiting on a person is not.
bool AgentsOwe(const std::string& path);

// The agents the session at `path` started, theirs beneath each, one line
// each: what /agents shows.
std::string AgentTree(const std::string& path);
}  // namespace uagent
#endif
