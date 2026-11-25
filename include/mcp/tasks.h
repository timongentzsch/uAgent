// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MCP_TASKS_H_
#define UAGENT_INCLUDE_MCP_TASKS_H_
// MCP tasks (io.modelcontextprotocol/tasks): followed inside the call that
// created one and, once it runs past a command's yield, as an activity the
// session's own thread polls.

#include <cstdint>
#include <string>

#include "include/core/json.h"
#include "include/mcp/server.h"
#include "include/tools/tool.h"

namespace uagent {

// The tools/call result `created` (resultType "task") followed to its end, or
// moved to the background; what the call returns.
ToolResult McpFollowTask(McpServer& server, const json& created,
                         const std::string& label, int64_t timeout,
                         const ToolContext& context, std::string& note);

// Polls the background tasks that are due, and asks to be woken for the next.
void McpPollTasks(McpRuntime& runtime);

// One tasks/cancel to each background task, without waiting for replies.
void McpCancelTasks(McpRuntime& runtime);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_TASKS_H_
