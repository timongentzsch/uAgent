// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_PROJECT_H_
#define UAGENT_INCLUDE_CORE_PROJECT_H_
// Startup instruction discovery: one AGENTS/CLAUDE file per directory from
// the repository root down. Memory discovery lives with memory storage.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/core/checked.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/strings.h"

namespace uagent {

struct ProjectInstructions {
  std::string text;
  std::string memory_index;
  std::string memory_always;
  std::vector<std::string> sources;
  std::vector<std::string> memory_sources;
  bool truncated = false;
  // Memory has its own byte cap, so it reports separately: a shared flag
  // would warn about the project-document limit for a memory overflow.
  bool memory_truncated = false;
  size_t memory_limit = 0;
};

inline std::filesystem::path ProjectRoot(const std::filesystem::path& cwd) {
  for (auto path = cwd;; path = path.parent_path()) {
    std::error_code ec;
    if (std::filesystem::exists(path / ".git", ec)) return path;
    if (path == path.root_path() || path.parent_path() == path) break;
  }
  return cwd;
}

// The instruction files a person writes: for every session or for a folder's
// coordinator, yours (every folder) or the project's. Always additive, read
// once when a session starts. Loading, showing, editing and protecting them
// all name them here.
inline std::filesystem::path InstructionPath(bool coordinator, bool project,
                                             const std::filesystem::path& cwd) {
  const char* name = coordinator ? "COORDINATOR.md" : "AGENTS.md";
  if (!project) return std::filesystem::path(GlobalBase()) / name;
  return coordinator ? cwd / ".uagent" / name : ProjectRoot(cwd) / name;
}

// Codex-style startup discovery: one instruction file per directory, ordered
// from repository root to cwd. CLAUDE.md is the fallback when no AGENTS file
// exists at that level. A coordinator then reads COORDINATOR.md: yours, then
// the folder's.
inline ProjectInstructions LoadProjectInstructions(
    const std::filesystem::path& cwd, size_t max_bytes,
    bool coordinator = false) {
  namespace fs = std::filesystem;
  ProjectInstructions loaded;
  if (max_bytes == 0) return loaded;

  std::vector<fs::path> dirs;
  fs::path root = ProjectRoot(cwd);
  for (fs::path path = cwd;; path = path.parent_path()) {
    dirs.push_back(path);
    if (path == root || path == path.root_path() ||
        path.parent_path() == path) {
      break;
    }
  }
  std::reverse(dirs.begin(), dirs.end());

  size_t used = 0;
  auto append = [&](const fs::path& path, std::string& destination,
                    const std::string& header = "") {
    std::optional<size_t> with_header = CheckedAdd(used, header.size());
    if (!with_header || *with_header >= max_bytes) {
      loaded.truncated = true;
      return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    size_t remaining = max_bytes - *with_header;
    std::string content;
    if (ReadBounded(input, remaining, content)) loaded.truncated = true;
    if (Trim(content).empty()) return false;
    if (!destination.empty()) destination += "\n\n";
    used = SaturatingAdd(used, header.size());
    used = SaturatingAdd(used, content.size());
    destination += header + content;
    return true;
  };
  // One file per directory, as Codex does. CLAUDE.md is a compatibility
  // fallback, not a second surface loaded alongside AGENTS.md.
  auto append_dir = [&](const fs::path& dir) {
    for (const char* name : {"AGENTS.override.md", "AGENTS.md", "CLAUDE.md"}) {
      std::error_code ec;
      fs::path candidate = dir / name;
      if (fs::is_regular_file(candidate, ec)) {
        if (append(candidate, loaded.text)) {
          loaded.sources.push_back(candidate.string());
        }
        break;
      }
    }
  };

  std::error_code ec;
  fs::path global = fs::weakly_canonical(UagentDir(""), ec);
  if (ec) global = UagentDir("");
  append_dir(global);
  for (const fs::path& dir : dirs) {
    if (dir != global) append_dir(dir);
  }
  if (coordinator) {
    for (bool project : {false, true}) {
      const fs::path path = InstructionPath(true, project, cwd);
      if (fs::is_regular_file(path, ec) &&
          append(path, loaded.text, "For the folder's coordinator:\n")) {
        loaded.sources.push_back(path.string());
      }
    }
  }
  return loaded;
}

// What instructions mean for `cwd`, in the order a session reads them: the
// four files a person edits (whether or not they exist yet), and every other
// file the loader picks up there (nested AGENTS.md, CLAUDE.md fallbacks).
inline json InstructionFiles(const std::filesystem::path& cwd) {
  json files = json::array();
  for (bool coordinator : {false, true}) {
    for (bool project : {false, true}) {
      const std::filesystem::path path =
          InstructionPath(coordinator, project, cwd);
      std::ifstream input(path, std::ios::binary);
      std::string text;
      if (input) ReadBounded(input, kProjectDocBytes, text);
      files.push_back({{"audience", coordinator ? "coordinator" : "sessions"},
                       {"scope", project ? "project" : "user"},
                       {"path", path.string()},
                       {"text", text}});
    }
  }
  json also = json::array();
  for (const std::string& source :
       LoadProjectInstructions(cwd, kProjectDocBytes).sources) {
    if (source != InstructionPath(false, false, cwd).string() &&
        source != InstructionPath(false, true, cwd).string()) {
      also.push_back(source);
    }
  }
  return {{"files", std::move(files)}, {"also_loaded", std::move(also)}};
}

// Replaces one instruction file; returns the error, or empty.
inline std::string WriteInstructionFile(bool coordinator, bool project,
                                        const std::filesystem::path& cwd,
                                        const std::string& text) {
  if (text.size() > kProjectDocBytes) return "instructions are at most 32 KiB";
  const std::filesystem::path path = InstructionPath(coordinator, project, cwd);
  // A project's AGENTS.md is a repository file others read; the rest live
  // in private .uagent directories.
  const bool shared = project && !coordinator;
  std::string error;
  if (!shared) CreatePrivateDirectories(path.parent_path());
  AtomicWriteFile(path.string(), text, shared ? 0644 : kPrivateFileMode, true,
                  error);
  return error;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_PROJECT_H_
