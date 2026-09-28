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

bool McpStartConfigured(McpServer& server, const RuntimeConfig& config,
                        int64_t& discovery_id, std::string& error);

// Spawn configured servers, handshake, and append one Tool per server tool.
// Spawns everything first and handshakes second, so slow server boots
// (npx downloads, node startup) overlap instead of adding up.
std::string McpRegister(std::vector<Tool>& tools, McpRuntime& runtime,
                        const RuntimeConfig& config,
                        const json& trusted_project = nullptr);

// Every configured server with its scope, state, tool count and, when it is
// not running, why. Published with the conversation state.
json McpStatus(const McpRuntime& runtime, const std::vector<Tool>& tools);

// Between turns: `mcp_restart` starts a server again, `mcp_enable` edits
// `disabled` in the file that defines it and starts or stops it now. A
// project file is only edited while it still matches the trusted copy, and
// trust is re-recorded for exactly that edit.
json McpControl(const json& request, std::vector<Tool>& tools,
                McpRuntime& runtime, const RuntimeConfig& config);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_REGISTER_H_
