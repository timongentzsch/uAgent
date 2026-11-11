// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_REGISTRY_H_
#define UAGENT_INCLUDE_TOOLS_REGISTRY_H_
// The built-in tools; each family is wired in its src/tools/registry_*.cc.

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "include/agent/activity_supervisor.h"
#include "include/core/fs.h"
#include "include/tools/tool.h"

namespace uagent {

// The sudo password for a host ("" for this machine); null when declined.
using RootPassword =
    std::function<std::shared_ptr<std::string>(const std::string& host)>;

std::vector<Tool> BuiltinTools(
    ActivitySupervisor& supervisor,
    const std::filesystem::path& workspace = CanonicalAccessPath("."),
    const RootPassword& root_password = nullptr);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_REGISTRY_H_
