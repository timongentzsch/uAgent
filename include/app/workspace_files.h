// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_WORKSPACE_FILES_H_
#define UAGENT_INCLUDE_APP_WORKSPACE_FILES_H_
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "include/core/json.h"
namespace uagent::workspace_files {
bool Shown(const std::filesystem::path& path);
json Folders(const std::string& path);
std::vector<std::string> Candidates(const std::filesystem::path& root,
                                    bool& more);
std::optional<std::filesystem::path> Resolve(const std::filesystem::path& root,
                                             const std::string& relative);
// Only filesystem operations. The main retains HTTP authentication, preview
// capabilities, CSP and rendering; both Local and remote use these handlers.
json Request(const std::filesystem::path& root, const json& request);
}  // namespace uagent::workspace_files
#endif
