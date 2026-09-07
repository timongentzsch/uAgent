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

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_REGISTER_H_
