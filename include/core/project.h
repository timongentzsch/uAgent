// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_PROJECT_H_
#define UAGENT_INCLUDE_CORE_PROJECT_H_
// Instruction files: which ones a session reads at startup, and reading and
// writing the ones a person edits.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "include/core/json.h"

namespace uagent {

struct ProjectInstructions {
  std::string text;
  std::string memory_index;
  std::string memory_always;
  std::vector<std::string> sources;
  std::vector<std::string> memory_sources;
  bool truncated = false;
  // Memory has its own byte cap, so its truncation reports separately.
  bool memory_truncated = false;
  size_t memory_limit = 0;
};

std::filesystem::path ProjectRoot(const std::filesystem::path& cwd);

struct GitWorktree {
  std::string git_dir;     // <main>/.git/worktrees/<name>
  std::string common_dir;  // <main>/.git
};
// A linked worktree's `.git` is a file naming its git directory and the shared
// one. Only a genuine pair is believed, or a hostile repository could name any
// folder and have it opened. Empty when no such file is found.
GitWorktree FindGitWorktree(const std::string& folder);
// Whether `dir` or a parent has a .git, as a directory or a worktree's file.
bool InGitRepository(const std::filesystem::path& dir);

// A directory contributes the first that exists, CLAUDE.md only where
// UAGENT_OTHER_AGENTS names claude. AGENTS.md is the one to create.
inline constexpr const char* kInstructionNames[] = {"AGENTS.override.md",
                                                    "AGENTS.md", "CLAUDE.md"};

std::filesystem::path InstructionFileIn(const std::filesystem::path& dir);

// The instruction file a person writes: yours (every folder) or the folder's.
std::filesystem::path InstructionPath(bool project,
                                      const std::filesystem::path& cwd);

// Scope "user" or "project"; false for anything else.
bool ParseInstructionScope(const std::string& scope, bool& project);

// An instruction file's text, bounded; empty when it does not exist.
std::string ReadInstructionFile(const std::filesystem::path& path);

// One instruction file per directory, ordered from repository root to cwd.
ProjectInstructions LoadProjectInstructions(const std::filesystem::path& cwd,
                                            size_t max_bytes);

// The instruction files for `cwd` in the order a session reads them: the two
// a person edits, whether or not they exist, then the others the loader finds.
json InstructionFiles(const std::filesystem::path& cwd);

// Replaces a file whose text is `current`; returns the error, or empty. With
// `base`, the text the editor started from, a file changed since is kept.
std::string WriteEditedFile(const std::filesystem::path& path,
                            const std::string& current, const std::string& text,
                            const std::optional<std::string>& base,
                            bool shared);

// Replaces one instruction file, as WriteEditedFile does.
std::string WriteInstructionFile(bool project, const std::filesystem::path& cwd,
                                 const std::string& text,
                                 const std::optional<std::string>& base = {});

// The stylesheet a person keeps for the web UI; it overrides design tokens.
std::filesystem::path UserStylePath();
inline constexpr size_t kUserStyleBytes = size_t{64} * 1024;
// Its text, empty when there is none.
std::string ReadUserStyle();

// The sections of the system message a person changed, in µAgent's private
// config folder: {"replace": {"## Scratch": "..."}, "append": "..."}.
std::filesystem::path SavedPromptPath();
inline constexpr size_t kSavedPromptBytes = size_t{64} * 1024;
std::string ReadSavedPrompt();
// Replaces it; empty text restores the built-in message.
std::string WriteSavedPrompt(const std::string& text,
                             const std::optional<std::string>& base = {});

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_PROJECT_H_
