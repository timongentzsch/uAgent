// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_REGISTRY_H_
#define UAGENT_INCLUDE_TOOLS_REGISTRY_H_
// Declarations for the built-in tool registry. Family wiring lives in
// src/tools/registry_{files,exec,activity,memory,todo}.cc (shared only via
// src/tools/registry_internal.h); registry.cc only orders the families, so
// consumers do not compile every tool body.

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "include/agent/activity_supervisor.h"
#include "include/core/fs.h"
#include "include/tools/tool.h"

namespace uagent {

// The sudo password for a host ("" for this machine), asked of the person at
// most once per turn; null when they decline. Without it, run offers no root.
using RootPassword =
    std::function<std::shared_ptr<std::string>(const std::string& host)>;

std::vector<Tool> BuiltinTools(
    ActivitySupervisor& supervisor,
    const std::filesystem::path& workspace = CanonicalAccessPath("."),
    RootPassword root_password = nullptr);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_REGISTRY_H_
