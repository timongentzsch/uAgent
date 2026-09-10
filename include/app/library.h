// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_LIBRARY_H_
#define UAGENT_INCLUDE_APP_LIBRARY_H_

#include <filesystem>
#include <string>

#include "include/core/file_watch.h"
#include "include/core/json.h"

namespace uagent {
// The same bounded document operations serve the terminal and web clients.
json LibraryControl(const json& request,
                    const std::filesystem::path& workspace);
json SkillControl(const json& request, const std::filesystem::path& workspace);
std::string LibraryChangePath();
void LibraryChanged();
bool LibraryPath(const std::filesystem::path& root,
                 const std::filesystem::path& path);
bool LibraryName(const std::string& name);
}  // namespace uagent
#endif
