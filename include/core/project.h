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

// A directory contributes one file, as in Codex: AGENTS.override.md, else
// AGENTS.md, else CLAUDE.md as a compatibility fallback. Where none exists,
// AGENTS.md is the one to create.
inline constexpr const char* kInstructionNames[] = {"AGENTS.override.md",
                                                    "AGENTS.md", "CLAUDE.md"};

std::filesystem::path InstructionFileIn(const std::filesystem::path& dir);

// The instruction files a person writes: for every session or for a folder's
// coordinator, yours (every folder) or the project's. Always additive, read
// once when a session starts. Loading, showing, editing and protecting them
// all name them here.
std::filesystem::path InstructionPath(bool coordinator, bool project,
                                      const std::filesystem::path& cwd);

// One of the four files, in the editors' words: audience "sessions" or
// "coordinator", scope "user" or "project". False for anything else.
bool ParseInstructionTarget(const std::string& audience,
                            const std::string& scope, bool& coordinator,
                            bool& project);

// An instruction file's text, bounded like the loader reads it; empty when
// it does not exist.
std::string ReadInstructionFile(const std::filesystem::path& path);

// Codex-style startup discovery: one instruction file per directory, ordered
// from repository root to cwd. CLAUDE.md is the fallback when no AGENTS file
// exists at that level. A coordinator then reads COORDINATOR.md: yours, then
// the folder's.
ProjectInstructions LoadProjectInstructions(const std::filesystem::path& cwd,
                                            size_t max_bytes,
                                            bool coordinator = false);

// What instructions mean for `cwd`, in the order a session reads them: the
// four files a person edits (whether or not they exist yet), and every other
// file the loader picks up there (nested AGENTS.md, CLAUDE.md fallbacks).
json InstructionFiles(const std::filesystem::path& cwd);

// Replaces one instruction file; returns the error, or empty. With `base`,
// the text the editor started from, a file changed since then is kept: two
// editors never silently overwrite each other.
std::string WriteInstructionFile(bool coordinator, bool project,
                                 const std::filesystem::path& cwd,
                                 const std::string& text,
                                 const std::optional<std::string>& base = {});

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_PROJECT_H_
