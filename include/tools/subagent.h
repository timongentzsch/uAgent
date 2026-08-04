// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_SUBAGENT_H_
#define UAGENT_INCLUDE_TOOLS_SUBAGENT_H_
// Delegation as a tool. Route descriptions and execution share one resolver so
// the model sees the choices the harness will actually accept.

#include <string>
#include <vector>

#include "include/api.h"
#include "include/providers.h"
#include "include/tools/process.h"
#include "include/tools/tool.h"

namespace uagent {

std::string DefaultTaskModel(const Api& api);
std::string DelegationRuntimeContext(const Api& api);

Tool SubagentTool(const Api& api, ProcessSupervisor& processes,
                  const std::vector<ModelRoute>& routes,
                  const std::vector<NamedProvider>& providers, bool debug);
Tool WaitAgentTool(ProcessSupervisor& processes);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_SUBAGENT_H_
