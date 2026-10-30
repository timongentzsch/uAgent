// Copyright 2026 Timon Gentzsch

#include "include/tools/registry.h"

#include <filesystem>
#include <utility>
#include <vector>

#include "src/tools/registry_internal.h"

namespace uagent {

std::vector<Tool> BuiltinTools(ActivitySupervisor& supervisor,
                               const std::filesystem::path& workspace,
                               RootPassword root_password) {
  std::vector<Tool> tools;
  RegisterFileTools(tools, supervisor, workspace);
  RegisterExecTools(tools, supervisor, workspace, std::move(root_password));
  RegisterActivityTool(tools, supervisor);
  RegisterMemoryTool(tools);
  RegisterTodoTool(tools);
  return tools;
}

}  // namespace uagent
