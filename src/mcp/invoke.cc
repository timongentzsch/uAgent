// Copyright 2026 Timon Gentzsch

#include "include/mcp/invoke.h"

#include <string>

#include "include/core/json.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/mcp/result.h"
#include "include/mcp/rpc.h"
#include "include/mcp/server.h"
#include "include/tools/tool.h"

namespace uagent {

ToolResult McpInvokeRemote(McpServer& server, const std::string& remote_name,
                           const json& arguments, int64_t timeout,
                           const ToolContext& context) {
  if (!server.alive) {
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "error: mcp server " + server.name + " has exited" +
                           McpStderrHint(server.name));
  }
  json response;
  if (RunCancellable([&] {
        json params = {{"name", remote_name}, {"arguments", arguments}};
        auto deadline = DeadlineAfter(context.RemainingSeconds(timeout));
        for (int round = 0; round < 8; ++round) {
          response = McpRpc(server, "tools/call", params,
                            SecondsUntil(deadline), true);
          const json* result = JsonObject(response, "result");
          if (!result ||
              JsonValue(*result, "resultType", "") != "input_required" ||
              AbortRequested()) {
            return;
          }
          if (!result->contains("requestState") &&
              !result->contains("inputRequests")) {
            return;
          }
          params.erase("requestState");
          params["inputResponses"] = json::object();
          if (result->contains("requestState")) {
            if (!(*result)["requestState"].is_string()) return;
            params["requestState"] = (*result)["requestState"];
          }
          if (const json* inputs = JsonObject(*result, "inputRequests")) {
            for (const auto& [key, input] : inputs->items()) {
              if (JsonValue(input, "method", "") != "roots/list") return;
              params["inputResponses"][key] = {{"roots", server.roots}};
            }
          } else if (result->contains("inputRequests")) {
            return;
          }
          if (SecondsUntil(deadline) <= 0) return;
        }
      })) {
    return ToolCancelled("error: call cancelled by user");
  }
  return McpResultText(server, response);
}

}  // namespace uagent
