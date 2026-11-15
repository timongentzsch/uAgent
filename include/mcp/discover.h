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

bool McpToolPageAllowed(const std::string& name, int64_t& pages,
                        std::set<std::string>& cursors,
                        const std::string& cursor);

bool McpConsumeToolPage(McpServer& server, const json& response, json& listed,
                        std::string& cursor);

bool McpFetchToolDefinitions(McpServer& s,
                             std::chrono::steady_clock::time_point deadline,
                             json& listed);

// Builds the whole replacement first, so a failed refresh keeps the old tools.
void McpReplaceServerTools(std::vector<Tool>& tools, McpServer& s,
                           const RuntimeConfig& config, const json& listed);

bool McpLoadServerTools(std::vector<Tool>& tools, McpServer& server,
                        const RuntimeConfig& config,
                        std::chrono::steady_clock::time_point deadline);

bool McpSendStartupToolPage(McpServer& server);

// Keeps one tools/list request outstanding across short startup windows.
bool McpAdvanceStartupTools(std::vector<Tool>& tools, McpServer& server,
                            const RuntimeConfig& config);

// Advances one optional server's startup; a missed deadline leaves it pending.
bool McpAdvanceStartup(std::vector<Tool>& tools, McpServer& server,
                       const RuntimeConfig& config);

// Call only between tool batches, when the agent holds no Tool pointers.
bool McpRefreshTools(std::vector<Tool>& tools, McpRuntime& runtime,
                     const RuntimeConfig& config,
                     std::chrono::steady_clock::time_point turn_deadline =
                         std::chrono::steady_clock::time_point::max());

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_DISCOVER_H_
