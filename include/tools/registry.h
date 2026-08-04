// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_REGISTRY_H_
#define UAGENT_INCLUDE_TOOLS_REGISTRY_H_
// Declarations for the built-in tool registry. Schema and handler wiring live
// in src/tools/registry.cc so consumers do not compile every tool body.

#include <filesystem>
#include <vector>

#include "include/core/fs.h"
#include "include/tools/process.h"
#include "include/tools/tool.h"

namespace uagent {

std::vector<Tool> BuiltinTools(
    ProcessSupervisor& supervisor,
    const std::filesystem::path& workspace = CanonicalAccessPath("."),
    bool inline_images = false, bool memory_generate = true);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_REGISTRY_H_
