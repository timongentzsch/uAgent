// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_MEMORY_H_
#define UAGENT_INCLUDE_TOOLS_MEMORY_H_

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "include/tools/tool.h"

namespace uagent {

struct MemoryEntry {
  std::string key;
  std::string path;
};

struct MemoryIndex {
  std::string text;
  std::vector<std::string> sources;
  bool truncated = false;
};

ToolResult ToolMemoryAction(const std::string& action, const std::string& key,
                            const std::optional<std::string>& content);

// The same storage map drives startup discovery, the tool, and /memory.
std::vector<MemoryEntry> ListMemories();
std::vector<MemoryEntry> ListMemories(const std::filesystem::path& cwd);
MemoryIndex LoadMemoryIndex(const std::filesystem::path& cwd, size_t max_bytes);
// Behavioral always-on slice: full content of global-scope memories, capped.
MemoryIndex LoadAlwaysOnMemory(const std::filesystem::path& cwd,
                               size_t max_bytes);
// Deterministic last line of defense for explicit writes.
std::string RedactMemorySecrets(std::string text);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_MEMORY_H_
