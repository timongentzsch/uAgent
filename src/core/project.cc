// Copyright 2026 Timon Gentzsch

#include "include/core/project.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "include/core/checked.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/skills.h"
#include "include/core/strings.h"

namespace uagent {

std::filesystem::path ProjectRoot(const std::filesystem::path& cwd) {
  for (auto path = cwd;; path = path.parent_path()) {
    std::error_code ec;
    if (std::filesystem::exists(path / ".git", ec)) return path;
    if (path == path.root_path() || path.parent_path() == path) break;
  }
  return cwd;
}

std::filesystem::path InstructionFileIn(const std::filesystem::path& dir) {
  for (const char* name : kInstructionNames) {
    // Claude's file speaks for a folder only where Claude is read from.
    if (std::string_view(name) == "CLAUDE.md" && !ReadsAgent("claude")) {
      continue;
    }
    std::error_code ec;
    if (std::filesystem::is_regular_file(dir / name, ec)) return dir / name;
  }
  return dir / "AGENTS.md";
}

std::filesystem::path InstructionPath(bool coordinator, bool project,
                                      const std::filesystem::path& cwd) {
  if (!coordinator) {
    return InstructionFileIn(project ? ProjectRoot(cwd)
                                     : std::filesystem::path(GlobalBase()));
  }
  return project ? cwd / ".uagent" / "COORDINATOR.md"
                 : std::filesystem::path(GlobalBase()) / "COORDINATOR.md";
}

bool ParseInstructionTarget(const std::string& audience,
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

std::string ReadInstructionFile(const std::filesystem::path& path) {
  return ReadFile(path.string(), kProjectDocBytes).value_or("");
}

ProjectInstructions LoadProjectInstructions(const std::filesystem::path& cwd,
                                            size_t max_bytes,
                                            bool coordinator) {
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

json InstructionFiles(const std::filesystem::path& cwd) {
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

std::string WriteInstructionFile(bool coordinator, bool project,
                                 const std::filesystem::path& cwd,
                                 const std::string& text,
                                 const std::optional<std::string>& base) {
  if (text.size() > kProjectDocBytes) {
    return "instructions are at most " +
           std::to_string(kProjectDocBytes / 1024) + " KiB";
  }
  const std::filesystem::path path = InstructionPath(coordinator, project, cwd);
  if (base && ReadInstructionFile(path) != *base) {
    return path.string() + " changed since it was opened; reload it first";
  }
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
