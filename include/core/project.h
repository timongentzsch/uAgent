// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_PROJECT_H_
#define UAGENT_INCLUDE_CORE_PROJECT_H_
// Instruction files: which ones a session reads at startup (one per
// directory from yours down to the working directory), and reading and
// writing the ones a person edits. Memory discovery lives with memory storage.

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

// A directory contributes one file, as in Codex: AGENTS.override.md, else
// AGENTS.md, else CLAUDE.md as a compatibility fallback. Where none exists,
// AGENTS.md is the one to create.
inline constexpr const char* kInstructionNames[] = {
    "AGENTS.override.md", "AGENTS.md", "CLAUDE.md"};

inline std::filesystem::path InstructionFileIn(
    const std::filesystem::path& dir) {
  for (const char* name : kInstructionNames) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(dir / name, ec)) return dir / name;
  }
  return dir / "AGENTS.md";
}

// The instruction files a person writes: for every session or for a folder's
// coordinator, yours (every folder) or the project's. Always additive, read
// once when a session starts. Loading, showing, editing and protecting them
// all name them here.
inline std::filesystem::path InstructionPath(bool coordinator, bool project,
                                             const std::filesystem::path& cwd) {
  if (!coordinator) {
    return InstructionFileIn(project ? ProjectRoot(cwd)
                                     : std::filesystem::path(GlobalBase()));
  }
  return project ? cwd / ".uagent" / "COORDINATOR.md"
                 : std::filesystem::path(GlobalBase()) / "COORDINATOR.md";
}

// One of the four files, in the editors' words: audience "sessions" or
// "coordinator", scope "user" or "project". False for anything else.
inline bool ParseInstructionTarget(const std::string& audience,
                                   const std::string& scope, bool& coordinator,
                                   bool& project) {
  if ((audience != "sessions" && audience != "coordinator") ||
      (scope != "user" && scope != "project")) {
    return false;
  }
  coordinator = audience == "coordinator";
  project = scope == "project";
  return true;
}

// An instruction file's text, bounded like the loader reads it; empty when
// it does not exist.
inline std::string ReadInstructionFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::string text;
  if (input) ReadBounded(input, kProjectDocBytes, text);
  return text;
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
  auto append_dir = [&](const fs::path& dir) {
    const fs::path file = InstructionFileIn(dir);
    std::error_code ec;
    if (fs::is_regular_file(file, ec) && append(file, loaded.text)) {
      loaded.sources.push_back(file.string());
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
      files.push_back({{"audience", coordinator ? "coordinator" : "sessions"},
                       {"scope", project ? "project" : "user"},
                       {"path", path.string()},
                       {"text", ReadInstructionFile(path)}});
    }
  }
  // The loader names paths canonically; compare them the same way.
  const auto same = [](const std::filesystem::path& a,
                       const std::filesystem::path& b) {
    std::error_code ec;
    return std::filesystem::weakly_canonical(a, ec) ==
           std::filesystem::weakly_canonical(b, ec);
  };
  json also = json::array();
  for (const std::string& source :
       LoadProjectInstructions(cwd, kProjectDocBytes).sources) {
    if (!same(source, InstructionPath(false, false, cwd)) &&
        !same(source, InstructionPath(false, true, cwd))) {
      also.push_back(source);
    }
  }
  return {{"files", std::move(files)}, {"also_loaded", std::move(also)}};
}

// Replaces one instruction file; returns the error, or empty.
inline std::string WriteInstructionFile(bool coordinator, bool project,
                                        const std::filesystem::path& cwd,
                                        const std::string& text) {
  if (text.size() > kProjectDocBytes) {
    return "instructions are at most " +
           std::to_string(kProjectDocBytes / 1024) + " KiB";
  }
  const std::filesystem::path path = InstructionPath(coordinator, project, cwd);
  // Written in place, never through a link a repository could plant.
  std::error_code ec;
  if (std::filesystem::is_symlink(path, ec)) {
    return path.string() + " is a symbolic link; edit its target instead";
  }
  // A project's files belong to its repository and others read them; yours
  // live in your private ~/.uagent. A project's .uagent stays private too.
  std::string error;
  if (!project || coordinator) CreatePrivateDirectories(path.parent_path());
  AtomicWriteFile(path.string(), text, project ? 0644 : kPrivateFileMode, true,
                  error);
  return error;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_PROJECT_H_
