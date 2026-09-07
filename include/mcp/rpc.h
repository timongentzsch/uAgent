// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MCP_RPC_H_
#define UAGENT_INCLUDE_MCP_RPC_H_
// Newline-delimited JSON-RPC framing. Writes drain the server's stdout
// while blocked, so a chatty server can never deadlock a large write.

#include <chrono>
#include <cstdint>
#include <string>

#include "include/core/json.h"
#include "include/mcp/server.h"

namespace uagent {

inline constexpr char kMcpModernProtocolVersion[] = "2026-07-28";

json McpRequestMeta();

// write a full message; drains the server's stdout while blocked so a chatty
// server can never deadlock a large write (both pipes full = classic hang)
bool McpWrite(McpServer& s, const std::string& data);

// read one newline-terminated message; cancellable waits also stop on Ctrl+C
bool McpReadLine(McpServer& s, std::string& line,
                 std::chrono::steady_clock::time_point deadline,
                 bool cancellable);

// id >= 0: request · id < 0: notification
bool McpSend(McpServer& s, int64_t id, const std::string& method,
             const json& params);

bool McpHandleMessage(McpServer& s, const json& message);

void McpFillAvailable(McpServer& s);

// Consume notifications that arrived while no request was outstanding. This
// is called at model-request boundaries so an idle server can change its tool
// list without first receiving a tools/call.
void McpDrainInbound(McpServer& s);

void McpDrainInbound(McpRuntime& runtime);

// Notifications are handled separately; stale responses are discarded.
json McpErrorReply(std::string message);

enum class McpResponseState : uint8_t { kPending, kReady, kClosed };

bool McpResponseMatches(const json& message, int64_t id);

// Consume only bytes already available. Deferred startup calls this at turn
// boundaries, so a slow optional server can become ready without adding a
// timeout to every model step. Notifications and server requests ahead of the
// target reply are handled before returning pending.
McpResponseState McpTryResponse(McpServer& s, int64_t id, json& response);

json McpAwait(McpServer& s, int64_t id, int64_t timeout_s, bool cancellable);

json McpRpc(McpServer& s, const std::string& method, const json& params,
            int64_t timeout_s, bool cancellable = false);

bool McpValidateDiscovery(const json& discovery, std::string& error);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_RPC_H_
