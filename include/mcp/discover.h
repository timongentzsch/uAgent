// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MCP_DISCOVER_H_
#define UAGENT_INCLUDE_MCP_DISCOVER_H_
// Tool and skill discovery: fetch a server's lists and fold its tools into
// the registry, replacing a previous generation in place.

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

bool McpPageAllowed(const std::string& name, const char* method, int64_t& pages,
                    std::set<std::string>& cursors, const std::string& cursor);

// One page of `method` (tools/list or skills/list) appended to `listed`.
bool McpConsumePage(McpServer& server, const char* method, const json& response,
                    json& listed, std::string& cursor);

bool McpFetchList(McpServer& s, const char* method,
                  std::chrono::steady_clock::time_point deadline, json& listed);

// Builds the whole replacement first, so a failed refresh keeps the old tools.
void McpReplaceServerTools(std::vector<Tool>& tools, McpServer& s,
                           const RuntimeConfig& config, const json& listed);

bool McpLoadServerTools(std::vector<Tool>& tools, McpServer& server,
                        const RuntimeConfig& config,
                        std::chrono::steady_clock::time_point deadline);

// Keeps one tools/list, then skills/list, request outstanding across short
// startup windows.
bool McpAdvanceStartupLists(std::vector<Tool>& tools, McpServer& server,
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
