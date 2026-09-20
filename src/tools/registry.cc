// Copyright 2026 Timon Gentzsch

#include "include/tools/registry.h"

#include <filesystem>
#include <utility>
#include <vector>

#include "include/tools/adapt_system.h"
#include "src/tools/registry_internal.h"

namespace uagent {

std::vector<Tool> BuiltinTools(ProcessSupervisor& supervisor,
                               const std::filesystem::path& workspace,
                               AdaptiveSystemState* adaptive_system) {
  std::vector<Tool> tools;
  if (adaptive_system) tools.push_back(AdaptSystemTool(*adaptive_system));
  RegisterFileTools(tools, supervisor, workspace);
  RegisterExecTools(tools, supervisor, workspace);
  RegisterActivityTool(tools, supervisor);
  RegisterMemoryTool(tools);
  return tools;
}

}  // namespace uagent
