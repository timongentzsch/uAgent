// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MCP_INVOKE_H_
#define UAGENT_INCLUDE_MCP_INVOKE_H_
// One bounded MCP invocation path.

#include <cstdint>
#include <string>

#include "include/core/json.h"
#include "include/mcp/server.h"
#include "include/tools/tool.h"

namespace uagent {

ToolResult McpInvokeRemote(McpServer& server, const std::string& remote_name,
                           const json& arguments, int64_t timeout,
                           const ToolContext& context);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_INVOKE_H_
