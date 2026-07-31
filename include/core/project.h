// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_PROJECT_H_
#define UAGENT_INCLUDE_CORE_PROJECT_H_
// Startup instruction discovery: one AGENTS/CLAUDE file per directory from
// the repository root down, then the memories the agent wrote itself.

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
  std::vector<std::string> sources;
  std::vector<std::string> memory_sources;
  bool truncated = false;
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
    std::string content(remaining + 1, '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    size_t read = static_cast<size_t>(input.gcount());
    content.resize(std::min(read, remaining));
    if (read > remaining || input.peek() != std::char_traits<char>::eof()) {
      loaded.truncated = true;
    }
    content = Utf8Prefix(std::move(content), remaining);
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

  // Only memory names/scopes enter the prompt. Bodies stay deferred behind the
  // memory tool and the shared budget always favors human instructions.
  auto append_memories = [&](const fs::path& base, const char* scope) {
    std::error_code list_error;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(base / kMemoryDir, list_error), end;
         it != end && !list_error; it.increment(list_error)) {
      if (it->path().extension() == ".md") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& file : files) {
      std::string entry =
          "- " + std::string(scope) + "/" + file.stem().string() + "\n";
      std::optional<size_t> total = CheckedAdd(used, entry.size());
      if (!total || *total > max_bytes) {
        loaded.truncated = true;
        break;
      }
      used = *total;
      loaded.memory_index += entry;
      loaded.memory_sources.push_back(file.string());
    }
  };

  std::error_code ec;
  fs::path global = fs::weakly_canonical(UagentDir(""), ec);
  if (ec) global = UagentDir("");
  append_dir(global);
  for (const fs::path& dir : dirs) {
    if (dir != global) append_dir(dir);
  }
  append_memories(GlobalBase(), "global");
  fs::path scoped = ProjectBase(cwd);
  if (fs::is_directory(scoped, ec) && scoped.string() != GlobalBase()) {
    append_memories(scoped, "project");
  }
  return loaded;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_PROJECT_H_
