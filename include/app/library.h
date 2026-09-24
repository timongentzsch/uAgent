// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_LIBRARY_H_
#define UAGENT_INCLUDE_APP_LIBRARY_H_

#include <filesystem>
#include <string>

#include "include/core/file_watch.h"
#include "include/core/json.h"
#include "include/core/library.h"

namespace uagent {
// The same bounded document operations serve the terminal and web clients.
json LibraryControl(const json& request,
                    const std::filesystem::path& workspace);
json SkillControl(const json& request, const std::filesystem::path& workspace);
}  // namespace uagent
#endif
