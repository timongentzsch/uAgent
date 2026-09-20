// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_SRC_TOOLS_REGISTRY_INTERNAL_H_
#define UAGENT_SRC_TOOLS_REGISTRY_INTERNAL_H_
// Module-private split of the built-in tool registry. BuiltinTools (see
// include/tools/registry.h) only orders the families; each family below owns
// its schemas, handlers and approval wiring verbatim. Families share nothing
// except the Tool vocabulary, so a file-tool change recompiles one TU.

#include <filesystem>
#include <vector>

#include "include/agent/process.h"
#include "include/tools/tool.h"

namespace uagent {

void RegisterFileTools(std::vector<Tool>& tools,
                       ProcessSupervisor& supervisor,
                       const std::filesystem::path& workspace);
void RegisterExecTools(std::vector<Tool>& tools,
                       ProcessSupervisor& supervisor,
                       const std::filesystem::path& workspace);
void RegisterActivityTool(std::vector<Tool>& tools,
                          ProcessSupervisor& supervisor);
void RegisterMemoryTool(std::vector<Tool>& tools);

}  // namespace uagent

#endif  // UAGENT_SRC_TOOLS_REGISTRY_INTERNAL_H_
