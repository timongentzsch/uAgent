// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_AGENT_TOOLS_H_
#define UAGENT_INCLUDE_APP_AGENT_TOOLS_H_
// The tools an agent has for the agents it starts: agent_start, agent_send,
// agents and decide.

#include <chrono>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "include/api/client.h"
#include "include/tools/tool.h"

namespace uagent {
// agent_start is added only while agents.depth allows; `own_model` names the
// model an agent starts on unless told otherwise; `judge_report` is what a
// wait returns of a report (Agent::ReportJudge).
void AddAgentTools(
    std::vector<Tool>& tools, const Api& api,
    std::function<std::string()> own_model,
    std::function<std::string(const std::string&, const std::string&, int64_t)>
        judge_report);

// The names of the agents the session at `path` started (with `calls`, only
// those these agent_start calls did) that still owe a report: those working
// or about to, not one waiting on a person. With `until` ahead, after
// waiting as the agents tool's wait does, or with `on_mail` false until none
// works.
std::vector<std::string> AgentsOwing(
    const std::string& path, const std::set<std::string>& calls = {},
    std::chrono::steady_clock::time_point until = {}, bool on_mail = true);

// Whether an agent of the session at `path` works or has not begun, or a
// report from one is not yet acknowledged. One waiting on a person does not
// count.
bool AgentsOwe(const std::string& path);

// The agents the session at `path` started, nested, one line each.
std::string AgentTree(const std::string& path);
}  // namespace uagent
#endif
