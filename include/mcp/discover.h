// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MCP_DISCOVER_H_
#define UAGENT_INCLUDE_MCP_DISCOVER_H_
// Tool discovery: fetch a server's definitions and fold them into the
// registry, replacing a previous generation in place.

#include <chrono>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "include/core/json.h"
#include "include/mcp/server.h"
#include "include/tools/tool.h"

namespace uagent {

struct RuntimeConfig;

bool McpToolPageAllowed(const std::string& name, int64_t max_pages,
                        int64_t& pages, std::set<std::string>& cursors,
                        const std::string& cursor);

bool McpConsumeToolPage(McpServer& server, const RuntimeConfig& config,
                        const json& response, json& listed,
                        std::string& cursor);

bool McpFetchToolDefinitions(McpServer& s, const RuntimeConfig& config,
                             std::chrono::steady_clock::time_point deadline,
                             json& listed);

// Build a complete replacement before touching the shared registry. A failed
// refresh therefore leaves every previously usable tool in place.
void McpReplaceServerTools(std::vector<Tool>& tools, McpServer& s,
                           const RuntimeConfig& config, const json& listed);

bool McpLoadServerTools(std::vector<Tool>& tools, McpServer& server,
                        const RuntimeConfig& config,
                        std::chrono::steady_clock::time_point deadline);

bool McpSendStartupToolPage(McpServer& server, const RuntimeConfig& config);

// Keep one tools/list request outstanding across short startup windows. A
// healthy slow optional server neither blocks startup nor loses its eventual
// reply to a newer request ID.
bool McpAdvanceStartupTools(std::vector<Tool>& tools, McpServer& server,
                            const RuntimeConfig& config);

// Advance one optional server without consuming its discovery reply in the
// idle-notification drain. A failed aggregate startup deadline leaves the
// state pending; the next turn resumes the same request for another bounded
// opportunity.
bool McpAdvanceStartup(std::vector<Tool>& tools, McpServer& server,
                       const RuntimeConfig& config);

// Apply notifications only between tool batches, when the agent holds no Tool
// pointers. Failed refreshes retain the prior registry and retry later.
bool McpRefreshTools(std::vector<Tool>& tools, McpRuntime& runtime,
                     const RuntimeConfig& config,
                     std::chrono::steady_clock::time_point turn_deadline =
                         std::chrono::steady_clock::time_point::max());

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_DISCOVER_H_
