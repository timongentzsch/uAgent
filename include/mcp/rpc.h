// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MCP_RPC_H_
#define UAGENT_INCLUDE_MCP_RPC_H_
// Newline-delimited JSON-RPC framing over a server's stdio.

#include <chrono>
#include <cstdint>
#include <string>

#include "include/core/json.h"
#include "include/mcp/server.h"

namespace uagent {

inline constexpr char kMcpModernProtocolVersion[] = "2026-07-28";

json McpRequestMeta();

// Drains the server's stdout while blocked: with both pipes full it would hang.
bool McpWrite(McpServer& s, const std::string& data,
              std::chrono::steady_clock::time_point deadline =
                  std::chrono::steady_clock::now() + std::chrono::seconds(10));

// One newline-terminated message; a cancellable wait also stops on Ctrl+C.
bool McpReadLine(McpServer& s, std::string& line,
                 std::chrono::steady_clock::time_point deadline,
                 bool cancellable);

// id >= 0: request · id < 0: notification
bool McpSend(McpServer& s, int64_t id, const std::string& method,
             const json& params,
             std::chrono::steady_clock::time_point deadline =
                 std::chrono::steady_clock::now() + std::chrono::seconds(10));

bool McpHandleMessage(McpServer& s, const json& message);

void McpFillAvailable(McpServer& s);

// Consumes notifications that arrived while no request was outstanding.
void McpDrainInbound(McpServer& s);

void McpDrainInbound(McpRuntime& runtime);

json McpErrorReply(std::string message);

enum class McpResponseState : uint8_t { kPending, kReady, kClosed };

bool McpResponseMatches(const json& message, int64_t id);

// Never blocks: consumes only bytes already available, handling other messages.
McpResponseState McpTryResponse(McpServer& s, int64_t id, json& response);

json McpAwait(McpServer& s, int64_t id, int64_t timeout_s, bool cancellable,
              std::chrono::steady_clock::time_point deadline =
                  std::chrono::steady_clock::time_point::max());

json McpRpc(McpServer& s, const std::string& method, const json& params,
            int64_t timeout_s, bool cancellable = false,
            std::chrono::steady_clock::time_point deadline =
                std::chrono::steady_clock::time_point::max());

bool McpValidateDiscovery(const json& discovery, std::string& error);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_RPC_H_
