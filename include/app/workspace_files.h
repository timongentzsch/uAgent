// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_WORKSPACE_FILES_H_
#define UAGENT_INCLUDE_APP_WORKSPACE_FILES_H_
#include <filesystem>
#include <optional>
#include <string>

#include "include/core/json.h"
namespace uagent::workspace_files {
json Folders(const std::string& path);
// The artifacts under `root`, newest first: {kind, key, name, path, place,
// count, pages, modified} each, and `alone` for one that is one file.
json Designs(const std::filesystem::path& root);
std::optional<std::filesystem::path> Resolve(const std::filesystem::path& root,
                                             const std::string& relative);
// Only filesystem operations. The main retains HTTP authentication, preview
// capabilities, CSP and rendering; both Local and remote use these handlers.
json Request(const std::filesystem::path& root, const json& request);
}  // namespace uagent::workspace_files
#endif
