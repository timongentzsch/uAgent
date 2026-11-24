// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_MEMORY_H_
#define UAGENT_INCLUDE_TOOLS_MEMORY_H_
// The memory store: the tool the model calls, the listing the prompt and
// /memory read, and the control requests clients send.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "include/agent/memory_store.h"
#include "include/tools/tool.h"

namespace uagent {

class Agent;
class Api;

ToolResult ToolMemoryAction(const std::string& action, const std::string& key,
                            const std::optional<std::string>& content);
json MemoryControl(const json& request, const std::filesystem::path& workspace);

// Native writable memories plus read-only Codex and Claude memory files.
std::vector<MemoryEntry> ListMemories();
std::vector<MemoryEntry> ListMemories(const std::filesystem::path& cwd,
                                      size_t limit = 0);
// Where Claude Code keeps the memories of the folder `cwd` belongs to.
std::filesystem::path ClaudeMemoryPath(const std::filesystem::path& cwd);
struct ObservedMemory {
  std::string revision;
  bool whole = false;
};
using ObservedMemories = std::map<std::string, ObservedMemory>;
// One line per memory within `max_bytes`, without the files in `inlined`.
MemoryIndex LoadMemoryIndex(const std::filesystem::path& cwd, size_t max_bytes,
                            const std::vector<std::string>& inlined = {},
                            ObservedMemories* observed = nullptr);
// The full content of global-scope memories, capped at `max_bytes`.
MemoryIndex LoadAlwaysOnMemory(const std::filesystem::path& cwd,
                               size_t max_bytes);

// One bounded extraction pass over an older idle session of `cwd`, as a side
// request of `agent`; "" or why none started.
std::string StartMemoryExtractor(Agent& agent, const Api& api,
                                 const std::filesystem::path& cwd,
                                 const std::string& current_session);
// Commit one reply against the evidence supplied to the extractor.
ToolResult SaveExtractedMemory(const std::string& key, const std::string& note,
                               const std::string& folded,
                               const std::filesystem::path& cwd,
                               const std::string& source,
                               const ObservedMemories& observed,
                               MemoryEvent& written);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_MEMORY_H_
