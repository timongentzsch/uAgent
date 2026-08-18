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

// Codex-style startup discovery: one instruction file per directory, ordered
// from repository root to cwd. CLAUDE.md is the fallback when no AGENTS file
// exists at that level.
inline ProjectInstructions LoadProjectInstructions(
    const std::filesystem::path& cwd, size_t max_bytes) {
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
  return loaded;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_PROJECT_H_
