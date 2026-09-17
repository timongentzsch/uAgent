// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_SUBAGENT_H_
#define UAGENT_INCLUDE_TOOLS_SUBAGENT_H_
// Delegation as a tool. Route descriptions and execution share one resolver so
// the model sees the choices the harness will actually accept.

#include <string>
#include <vector>

#include "include/api.h"
#include "include/core/json.h"
#include "include/providers.h"
#include "include/tools/process.h"
#include "include/tools/tool.h"

namespace uagent {

class CollaboratorRuntime;

std::string DefaultSubagentModel(const Api& api);
// The workspace's collaborator records, one object each: id, name,
// description, model, mode, status, and the activity id when the child is
// still running. Shared with the TUI so `/agents` and the tool's `list`
// operation cannot drift apart.
std::vector<json> CollaboratorSummaries(
    const ProcessSupervisor& processes,
    const CollaboratorRuntime* runtime = nullptr);
json InspectCollaborator(const ProcessSupervisor& processes,
                         const std::string& id,
                         const json& request = json::object(),
                         const CollaboratorRuntime* runtime = nullptr);
ToolResult MessageCollaborator(const ProcessSupervisor& processes,
                               CollaboratorRuntime* runtime,
                               const std::string& id, const std::string& text,
                               const std::string& from = "", int hops = 0);
std::string DelegationRuntimeContext(const Api& api);

Tool SubagentTool(const Api& api, ProcessSupervisor& processes,
                  const std::vector<ModelRoute>& routes,
                  const std::vector<NamedProvider>& providers, bool debug,
                  CollaboratorRuntime* runtime = nullptr);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_SUBAGENT_H_
