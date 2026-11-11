// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_PROJECT_H_
#define UAGENT_INCLUDE_CORE_PROJECT_H_
// Instruction files: which ones a session reads at startup (one per
// directory from yours down to the working directory), and reading and
// writing the ones a person edits. Memory discovery lives with memory storage.

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
  // Memory has its own byte cap, so it reports separately: a shared flag
  // would warn about the project-document limit for a memory overflow.
  bool memory_truncated = false;
  size_t memory_limit = 0;
};

std::filesystem::path ProjectRoot(const std::filesystem::path& cwd);

struct GitWorktree {
  std::string git_dir;     // <main>/.git/worktrees/<name>
  std::string common_dir;  // <main>/.git
};
// A linked worktree keeps its git directory, and the repository's shared one,
// outside the folder: its `.git` is a file naming them. Only a genuine pair is
// believed -- the directory must be a child of the shared one's `worktrees`
// and point back at this `.git` -- because a hostile repository could
// otherwise name any folder and have it opened. Empty when `folder` and the
// folders above it hold no such file.
GitWorktree FindGitWorktree(const std::string& folder);
// Whether `dir` is in a git repository: it or a parent has a .git, as a
// directory or, in a worktree, as a file.
bool InGitRepository(const std::filesystem::path& dir);

// A directory contributes one file, as in Codex: AGENTS.override.md, else
// AGENTS.md, else CLAUDE.md where UAGENT_OTHER_AGENTS names claude. Where
// none exists, AGENTS.md is the one to create.
inline constexpr const char* kInstructionNames[] = {"AGENTS.override.md",
                                                    "AGENTS.md", "CLAUDE.md"};

std::filesystem::path InstructionFileIn(const std::filesystem::path& dir);

// The instruction files a person writes: yours (every folder) or the
// folder's. Always additive, read once when a session starts. Loading,
// showing, editing and protecting them all name them here.
std::filesystem::path InstructionPath(bool project,
                                      const std::filesystem::path& cwd);

// One of the two files, in the editors' words: scope "user" or "project".
// False for anything else.
bool ParseInstructionScope(const std::string& scope, bool& project);

// An instruction file's text, bounded like the loader reads it; empty when
// it does not exist.
std::string ReadInstructionFile(const std::filesystem::path& path);

// Codex-style startup discovery: one instruction file per directory, ordered
// from repository root to cwd. CLAUDE.md is the fallback when no AGENTS file
// exists at that level.
ProjectInstructions LoadProjectInstructions(const std::filesystem::path& cwd,
                                            size_t max_bytes);

// What instructions mean for `cwd`, in the order a session reads them: the
// two files a person edits (whether or not they exist yet), and every other
// file the loader picks up there (nested AGENTS.md, CLAUDE.md fallbacks).
json InstructionFiles(const std::filesystem::path& cwd);

// Replaces a file a person edits, given its text now (`current`); returns
// the error, or empty. With `base`, the text the editor started from, a file
// changed since then is kept: two editors never silently overwrite each
// other. A `shared` file is a folder's, which others read; any other is
// created private.
std::string WriteEditedFile(const std::filesystem::path& path,
                            const std::string& current, const std::string& text,
                            const std::optional<std::string>& base,
                            bool shared);

// Replaces one instruction file, as WriteEditedFile does.
std::string WriteInstructionFile(bool project, const std::filesystem::path& cwd,
                                 const std::string& text,
                                 const std::optional<std::string>& base = {});

// The stylesheet a person keeps for the web UI, in µAgent's private config
// folder. It overrides the app's design tokens: the web host serves it at
// /api/user.css, and the app adds it after its own sheets. An empty one is
// the default look.
std::filesystem::path UserStylePath();
inline constexpr size_t kUserStyleBytes = size_t{64} * 1024;
// Its text, empty when there is none.
std::string ReadUserStyle();

// The sections of the system message a person changed, in µAgent's private
// config folder: {"replace": {"## Scratch": "..."}, "append": "..."}.
// Sessions read it when they start.
std::filesystem::path SavedPromptPath();
inline constexpr size_t kSavedPromptBytes = size_t{64} * 1024;
std::string ReadSavedPrompt();
// Replaces it; empty text restores the built-in message.
std::string WriteSavedPrompt(const std::string& text,
                             const std::optional<std::string>& base = {});

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_PROJECT_H_
