// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_MEMORY_H_
#define UAGENT_INCLUDE_TOOLS_MEMORY_H_
// The memory store: the tool the model calls, the listing the prompt and
// /memory read, and the control requests clients send.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "include/agent/memory_store.h"
#include "include/tools/tool.h"

namespace uagent {

class Api;
class ActivitySupervisor;

ToolResult ToolMemoryAction(const std::string& action, const std::string& key,
                            const std::optional<std::string>& content);
json MemoryControl(const json& request, const std::filesystem::path& workspace);

// Native writable memories plus read-only Codex/Claude memory files drive
// startup discovery, the tool, and /memory.
std::vector<MemoryEntry> ListMemories();
std::vector<MemoryEntry> ListMemories(const std::filesystem::path& cwd,
                                      size_t limit = 0);
// Where Claude Code keeps the memories of the folder `cwd` belongs to.
std::filesystem::path ClaudeMemoryPath(const std::filesystem::path& cwd);
// One line per memory within `max_bytes`, leaving out the files `inlined`
// already shows whole.
MemoryIndex LoadMemoryIndex(const std::filesystem::path& cwd, size_t max_bytes,
                            const std::vector<std::string>& inlined = {});
// Behavioral always-on slice: full content of global-scope memories, capped.
MemoryIndex LoadAlwaysOnMemory(const std::filesystem::path& cwd,
                               size_t max_bytes);

// One bounded idle-session extraction pass; the child reads only the source and
// receives only the memory tool.
std::string StartMemoryExtractor(ActivitySupervisor& processes, const Api& api,
                                 const std::filesystem::path& cwd,
                                 const std::string& current_session);
// Called by that child as it ends: the session it read is marked done, or
// released to be tried again.
void SettleMemoryClaim(bool extracted);
bool BuildMemoryExtractionPrompt(const std::string& source,
                                 const std::filesystem::path& cwd,
                                 std::string& prompt, std::string& error);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_MEMORY_H_
