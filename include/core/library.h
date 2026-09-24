// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_CORE_LIBRARY_H_
#define UAGENT_INCLUDE_CORE_LIBRARY_H_

#include <filesystem>
#include <string>

namespace uagent {
// Shared rules for the managed library of memories, skills and prompts.
// Writers touch the change marker so watching clients reload.
std::string LibraryChangePath();
void LibraryChanged();
// True when `path` lies strictly inside `root` with no symlinked component.
bool LibraryPath(const std::filesystem::path& root,
                 const std::filesystem::path& path);
// A single safe path component within the library name bound.
bool LibraryName(const std::string& name);
}  // namespace uagent
#endif  // UAGENT_INCLUDE_CORE_LIBRARY_H_
