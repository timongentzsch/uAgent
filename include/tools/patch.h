// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_PATCH_H_
#define UAGENT_INCLUDE_TOOLS_PATCH_H_

#include <string>
#include <vector>

#include "include/tools/tool.h"

namespace uagent {

// The changes `patch` asks for, in its order, each as the edit_file,
// write_file or delete_file call that makes it; none and an `error` when it
// is not a patch. Each hunk is an exact replacement.
std::vector<ToolPart> ParsePatch(const std::string& patch, std::string& error);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_PATCH_H_
