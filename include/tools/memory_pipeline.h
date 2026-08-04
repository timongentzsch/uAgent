// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_MEMORY_PIPELINE_H_
#define UAGENT_INCLUDE_TOOLS_MEMORY_PIPELINE_H_

#include <filesystem>
#include <string>

namespace uagent {

class Api;
class ProcessSupervisor;

// Starts at most one bounded, idle-session consolidation child. Empty means
// either no eligible work or successful start; diagnostics are debug-only.
std::string StartMemoryPipeline(ProcessSupervisor& processes, const Api& api,
                                const std::filesystem::path& cwd,
                                const std::string& current_session, bool debug);

// Used by the hidden child entrypoint before normal agent bootstrap.
bool BuildMemoryConsolidationPrompt(const std::string& source,
                                    const std::filesystem::path& cwd,
                                    std::string& prompt, std::string& error);
bool MarkMemorySessionProcessed(const std::string& source, std::string& error);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_MEMORY_PIPELINE_H_
