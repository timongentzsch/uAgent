// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MCP_REGISTER_H_
#define UAGENT_INCLUDE_MCP_REGISTER_H_
// Configured server startup, discovery, and lifecycle.

#include <string>
#include <vector>

#include "include/core/json.h"
#include "include/mcp/server.h"
#include "include/tools/tool.h"

namespace uagent {

struct RuntimeConfig;

bool McpStartConfigured(McpServer& server, int64_t& discovery_id,
                        std::string& error);

// Spawns every configured server before any handshake, so slow boots overlap.
std::string McpRegister(std::vector<Tool>& tools, McpRuntime& runtime,
                        const RuntimeConfig& config,
                        const json& trusted_project = nullptr);

// Every configured server with its scope, state, tool count and error.
json McpStatus(const McpRuntime& runtime, const std::vector<Tool>& tools);

// Between turns: `mcp_restart` a server, or `mcp_enable` edits its `disabled`.
json McpControl(const json& request, std::vector<Tool>& tools,
                McpRuntime& runtime, const RuntimeConfig& config);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_REGISTER_H_
