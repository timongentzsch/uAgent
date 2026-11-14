// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_SRC_TOOLS_REGISTRY_INTERNAL_H_
#define UAGENT_SRC_TOOLS_REGISTRY_INTERNAL_H_
// The tool families BuiltinTools orders; each owns its schemas, handlers and
// approval wiring.

#include <filesystem>
#include <vector>

#include "include/agent/activity_supervisor.h"
#include "include/tools/registry.h"
#include "include/tools/tool.h"

namespace uagent {

void RegisterFileTools(std::vector<Tool>& tools, ActivitySupervisor& supervisor,
                       const std::filesystem::path& workspace);
void RegisterExecTools(std::vector<Tool>& tools, ActivitySupervisor& supervisor,
                       const std::filesystem::path& workspace,
                       const RootPassword& root_password);
void RegisterActivityTool(std::vector<Tool>& tools,
                          ActivitySupervisor& supervisor);
void RegisterMemoryTool(std::vector<Tool>& tools);
void RegisterTodoTool(std::vector<Tool>& tools);

}  // namespace uagent

#endif  // UAGENT_SRC_TOOLS_REGISTRY_INTERNAL_H_
