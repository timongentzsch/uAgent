// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_REGISTRY_H_
#define UAGENT_INCLUDE_TOOLS_REGISTRY_H_
// Declarations for the built-in tool registry. Family wiring lives in
// src/tools/registry_{files,exec,activity,memory}.cc (shared only via
// src/tools/registry_internal.h); registry.cc only orders the families, so
// consumers do not compile every tool body.

#include <filesystem>
#include <vector>

#include "include/agent/adaptive_system.h"
#include "include/agent/process.h"
#include "include/core/fs.h"
#include "include/tools/tool.h"

namespace uagent {

std::vector<Tool> BuiltinTools(
    ProcessSupervisor& supervisor,
    const std::filesystem::path& workspace = CanonicalAccessPath("."),
    AdaptiveSystemState* adaptive_system = nullptr);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_REGISTRY_H_
