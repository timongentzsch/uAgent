// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_MEMORY_H_
#define UAGENT_INCLUDE_TOOLS_MEMORY_H_

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "include/agent/memory_store.h"
#include "include/tools/tool.h"

namespace uagent {

class Api;
class ProcessSupervisor;

ToolResult ToolMemoryAction(const std::string& action, const std::string& key,
                            const std::optional<std::string>& content);
json MemoryControl(const json& request, const std::filesystem::path& workspace);

// Native writable memories plus read-only Codex/Claude memory files drive
// startup discovery, the tool, and /memory.
std::vector<MemoryEntry> ListMemories();
std::vector<MemoryEntry> ListMemories(const std::filesystem::path& cwd,
                                      size_t limit = 0);
MemoryIndex LoadMemoryIndex(const std::filesystem::path& cwd, size_t max_bytes);
// Behavioral always-on slice: full content of global-scope memories, capped.
MemoryIndex LoadAlwaysOnMemory(const std::filesystem::path& cwd,
                               size_t max_bytes);

// One bounded idle-session extraction job; the child reads only the source and
// receives only the memory tool.
std::string StartMemoryExtractor(ProcessSupervisor& processes, const Api& api,
                                 const std::filesystem::path& cwd,
                                 const std::string& current_session);
bool BuildMemoryExtractionPrompt(const std::string& source,
                                 const std::filesystem::path& cwd,
                                 std::string& prompt, std::string& error);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_MEMORY_H_
