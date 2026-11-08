// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_CORE_LIBRARY_H_
#define UAGENT_INCLUDE_CORE_LIBRARY_H_
// Rules shared by the managed library of memories and skills: its change
// marker, path checks and write lease.

#include <filesystem>
#include <string>

#include "include/core/file_watch.h"
#include "include/core/json.h"
#include "include/core/lease.h"

namespace uagent {
// Shared rules for the managed library of memories, skills and prompts.
// Writers touch the change marker so watching clients reload.
std::string LibraryChangePath();
void LibraryChanged();
// True when `path` lies strictly inside `root` with no symlinked component.
bool LibraryPath(const std::filesystem::path& root,
                 const std::filesystem::path& path);
// Serializes writers of library documents across processes.
bool AcquireLibraryWriteLease(FileLease& lease, std::string& error);
// A single safe path component within the library name bound.
bool LibraryName(const std::string& name);
// The document at `path` as a write request last read it: read (at most
// `cap` bytes) into `previous`, `exists` set, and checked against the
// request's `revision`. An error reply when it cannot be read, or when it
// changed since (`conflict`, naming the `noun`); empty when the write may go
// ahead.
json CheckRevision(const json& request, const std::filesystem::path& path,
                   size_t cap, const std::string& noun, std::string& previous,
                   bool& exists);
}  // namespace uagent
#endif  // UAGENT_INCLUDE_CORE_LIBRARY_H_
