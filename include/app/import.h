// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_IMPORT_H_
#define UAGENT_INCLUDE_APP_IMPORT_H_
// Importing skills and memories that Claude Code and Codex keep.

#include <filesystem>
#include <string>

#include "include/core/json.h"

namespace uagent {
// `list` shows what Claude Code and Codex keep; `run` (key: an item's key or
// name, or "all") copies it. `only` limits both to "skill" or "memory".
json ImportControl(const json& request, const std::filesystem::path& workspace);
// A SKILL.md with vendor tool references turned into µAgent's, plus a
// requires-tools line for the ones found.
std::string RewriteToolReferences(const std::string& skill_md);
}  // namespace uagent
#endif
