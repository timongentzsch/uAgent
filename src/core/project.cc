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
#include "include/core/defaults.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/sandbox.h"
#include "include/core/skills.h"
#include "include/core/strings.h"
#include "include/core/workspace.h"

namespace uagent {

std::filesystem::path ProjectRoot(const std::filesystem::path& cwd) {
  for (auto path = cwd;; path = path.parent_path()) {
    std::error_code ec;
    if (std::filesystem::exists(path / ".git", ec)) return path;
    if (path == path.root_path() || path.parent_path() == path) break;
  }
  return cwd;
}

bool InGitRepository(const std::filesystem::path& dir) {
  std::error_code ec;
  return std::filesystem::exists(ProjectRoot(dir) / ".git", ec);
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

std::filesystem::path InstructionPath(bool project,
                                      const std::filesystem::path& cwd) {
  if (project && RemoteWorkspace()) {
    return JsonValue(
        WorkspaceRequest({{"op", "instructions"}, {"action", "path"}}), "path",
        Workspace().path + "/AGENTS.md");
  }
  return InstructionFileIn(project ? ProjectRoot(cwd)
                                   : std::filesystem::path(GlobalBase()));
}

bool ParseInstructionScope(const std::string& scope, bool& project) {
  project = scope == "project";
  return project || scope == "user";
}

std::string ReadInstructionFile(const std::filesystem::path& path) {
  if (RemoteWorkspace() && !SandboxPathWithin(path.string(), GlobalBase())) {
    std::string bytes, error;
    ReadWorkspaceFile(path.string(), ProjectDocBytes(), bytes, error);
    return bytes;
  }
  return ReadFile(path.string(), ProjectDocBytes()).value_or("");
}

ProjectInstructions LoadProjectInstructions(const std::filesystem::path& cwd,
                                            size_t max_bytes) {
  namespace fs = std::filesystem;
  ProjectInstructions loaded;
  if (max_bytes == 0) return loaded;
  if (RemoteWorkspace()) {
    const auto user = InstructionPath(false, cwd);
    loaded.text = Utf8Prefix(ReadInstructionFile(user), max_bytes);
    if (!loaded.text.empty()) loaded.sources.push_back(user.string());
    const json project = WorkspaceRequest({{"op", "instructions"}});
    const std::string text = JsonValue(project, "text", "");
    const size_t remaining = max_bytes - loaded.text.size();
    if (remaining && !text.empty()) {
      loaded.text += "\n\n" + Utf8Prefix(text, remaining);
    }
    loaded.truncated =
        text.size() > remaining || JsonValue(project, "truncated", false);
    if (const json* sources = JsonArray(project, "sources")) {
      for (const auto& source : *sources) {
        if (source.is_string()) {
          loaded.sources.push_back(source.get<std::string>());
        }
      }
    }
    return loaded;
  }

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
  auto append = [&](const fs::path& path, std::string& destination) {
    if (used >= max_bytes) {
      loaded.truncated = true;
      return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::string content;
    if (ReadBounded(input, max_bytes - used, content)) loaded.truncated = true;
    if (Trim(content).empty()) return false;
    if (!destination.empty()) destination += "\n\n";
    used = SaturatingAdd(used, content.size());
    destination += content;
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
  return loaded;
}

json InstructionFiles(const std::filesystem::path& cwd) {
  if (RemoteWorkspace()) {
    json result =
        WorkspaceRequest({{"op", "instructions"}, {"action", "files"}});
    if (json* files = result.contains("files") && result["files"].is_array()
                          ? &result["files"]
                          : nullptr) {
      for (auto it = files->begin(); it != files->end();) {
        if (JsonValue(*it, "scope", "") == "user") {
          it = files->erase(it);
        } else {
          ++it;
        }
      }
      const auto user = InstructionPath(false, cwd);
      files->insert(files->begin(), json{{"scope", "user"},
                                         {"path", user.string()},
                                         {"text", ReadInstructionFile(user)}});
    }
    return result;
  }
  json files = json::array();
  for (bool project : {false, true}) {
    const std::filesystem::path path = InstructionPath(project, cwd);
    files.push_back({{"scope", project ? "project" : "user"},
                     {"path", path.string()},
                     {"text", ReadInstructionFile(path)}});
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
       LoadProjectInstructions(cwd, ProjectDocBytes()).sources) {
    if (!same(source, InstructionPath(false, cwd)) &&
        !same(source, InstructionPath(true, cwd))) {
      also.push_back(source);
    }
  }
  return {{"files", std::move(files)}, {"also_loaded", std::move(also)}};
}

namespace {
// Replaces a file a person edits, given its text as they saw it (`current`).
// With `base`, a file changed since the editor opened it is kept: two
// editors never silently overwrite each other.
std::string WriteEditedFile(const std::filesystem::path& path,
                            const std::string& current, const std::string& text,
                            const std::optional<std::string>& base,
                            bool shared) {
  if (base && current != *base) {
    return path.string() + " changed since it was opened; reload it first";
  }
  // Written in place, never through a link a repository could plant.
  std::error_code ec;
  if (std::filesystem::is_symlink(path, ec)) {
    return path.string() + " is a symbolic link; edit its target instead";
  }
  // A folder's files belong to its repository and others read them; yours
  // live in your private ~/.uagent.
  std::string error;
  if (!shared) CreatePrivateDirectories(path.parent_path());
  AtomicWriteFile(path.string(), text,
                  shared ? kSharedFileMode : kPrivateFileMode, true, error);
  return error;
}

}  // namespace

std::string WriteInstructionFile(bool project, const std::filesystem::path& cwd,
                                 const std::string& text,
                                 const std::optional<std::string>& base) {
  if (project && RemoteWorkspace()) {
    json request = {
        {"op", "instructions"}, {"action", "write"}, {"text", text}};
    if (base) request["base"] = *base;
    return JsonValue(WorkspaceRequest(request), "error", "");
  }
  if (text.size() > ProjectDocBytes()) {
    return "instructions are at most " +
           std::to_string(ProjectDocBytes() / 1024) + " KiB";
  }
  const std::filesystem::path path = InstructionPath(project, cwd);
  return WriteEditedFile(path, ReadInstructionFile(path), text, base, project);
}

std::filesystem::path UserStylePath() {
  return std::filesystem::path(UagentDir(kConfigDir)) / "style.css";
}

std::string ReadUserStyle() {
  return ReadFile(UserStylePath().string(), kUserStyleBytes).value_or("");
}

std::string WriteUserStyle(const std::string& text,
                           const std::optional<std::string>& base) {
  if (text.size() > kUserStyleBytes) {
    return "the stylesheet is at most " +
           std::to_string(kUserStyleBytes / 1024) + " KiB";
  }
  return WriteEditedFile(UserStylePath(), ReadUserStyle(), text, base,
                         /*shared=*/false);
}

namespace {
std::string TrimLine(std::string_view text) {
  return Trim(std::string(text.substr(0, text.find_first_of("\r\n"))));
}
}  // namespace

GitWorktree FindGitWorktree(const std::string& folder) {
  namespace fs = std::filesystem;
  std::error_code error;
  for (fs::path dir = folder; dir.has_relative_path() && dir != dir.root_path();
       dir = dir.parent_path()) {
    const fs::path dot_git = dir / ".git";
    if (!fs::exists(dot_git, error)) continue;
    if (!fs::is_regular_file(dot_git, error)) return {};
    const std::string line =
        TrimLine(ReadFile(dot_git.string(), 4096).value_or(""));
    constexpr std::string_view kPrefix = "gitdir:";
    if (!line.starts_with(kPrefix)) return {};
    fs::path git_dir = Trim(line.substr(kPrefix.size()));
    if (git_dir.is_relative()) git_dir = dir / git_dir;
    git_dir = CanonicalAccessPath(git_dir.string());
    const fs::path worktrees = git_dir.parent_path();
    const fs::path common_dir = worktrees.parent_path();
    auto line_of = [&](const char* name) {
      return TrimLine(ReadFile((git_dir / name).string(), 4096).value_or(""));
    };
    fs::path shared = line_of("commondir");
    if (shared.is_relative()) shared = git_dir / shared;
    if (worktrees.filename() != "worktrees" ||
        !fs::is_directory(common_dir / "objects", error) ||
        CanonicalAccessPath(shared.string()) != common_dir ||
        CanonicalAccessPath(line_of("gitdir")) !=
            CanonicalAccessPath(dot_git.string())) {
      return {};
    }
    return {git_dir.string(), common_dir.string()};
  }
  return {};
}

}  // namespace uagent
