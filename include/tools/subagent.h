// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_SUBAGENT_H_
#define UAGENT_INCLUDE_TOOLS_SUBAGENT_H_
// Delegation as a tool. Route descriptions and execution share one resolver so
// the model sees the choices the harness will actually accept. Every child is
// an ordinary saved session in this workspace's history whose header names
// its parent and role; it is addressed by that parent and never listed as a
// session of its own.

#include <string>
#include <vector>

#include "include/agent/process.h"
#include "include/api.h"
#include "include/core/json.h"
#include "include/providers.h"
#include "include/tools/tool.h"

namespace uagent {

// This session's children, one object each: id, name, description, model,
// mode, label, status, and the activity id while one runs. Shared with the
// TUI and the web so `/agents` and the tool's `list` cannot drift apart.
std::vector<json> AgentSummaries(const ProcessSupervisor& processes);
json InspectAgent(const ProcessSupervisor& processes, const std::string& id,
                  const json& request = json::object());
// Guidance a running child reads at its next step, or an idle one at its next
// followup.
ToolResult MessageAgent(const ProcessSupervisor& processes,
                        const std::string& id, const std::string& text);

Tool SubagentTool(const Api& api, ProcessSupervisor& processes,
                  const std::vector<ModelRoute>& routes,
                  const std::vector<NamedProvider>& providers, bool debug);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_SUBAGENT_H_
