// Copyright 2026 Timon Gentzsch

#include "include/tools/registry.h"

#include <filesystem>
#include <vector>

#include "src/tools/registry_internal.h"

namespace uagent {

std::vector<Tool> BuiltinTools(ProcessSupervisor& supervisor,
                               const std::filesystem::path& workspace) {
  std::vector<Tool> tools;
  RegisterFileTools(tools, supervisor, workspace);
  RegisterExecTools(tools, supervisor, workspace);
  RegisterActivityTool(tools, supervisor);
  RegisterMemoryTool(tools);
  return tools;
}

}  // namespace uagent
