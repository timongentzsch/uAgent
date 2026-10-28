// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_IMPORT_H_
#define UAGENT_INCLUDE_APP_IMPORT_H_
// Importing skills and memories that Claude Code and Codex keep.

#include <filesystem>
#include <string>

#include "include/core/json.h"

namespace uagent {
// What Claude Code and Codex keep that µAgent can take over. `list` shows it;
// `run` (key: an item's key or name, or "all") copies it into µAgent's own
// library. `only` limits both to "skill" or "memory".
json ImportControl(const json& request, const std::filesystem::path& workspace);
// A SKILL.md with the vendor tool names that are clearly tool references
// (`Bash`, "the Read tool", allowed-tools entries) turned into µAgent's, and a
// requires-tools line for the ones found.
std::string RewriteToolReferences(const std::string& skill_md);
}  // namespace uagent
#endif
