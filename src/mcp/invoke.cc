// Copyright 2026 Timon Gentzsch

#include "include/mcp/invoke.h"

#include <algorithm>
#include <string>

#include "include/core/json.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/mcp/elicit.h"
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
                       "mcp server " + server.name + " has exited" +
                           McpStderrHint(server.name));
  }
  json response;
  std::string note;
  if (RunCancellable([&] {
        json params = {{"name", remote_name}, {"arguments", arguments}};
        bool wait = false;
        for (int round = 0; round < 8; ++round) {
          // Each request has its own budget, so a person's time answering is
          // never the server's; a retry after an accepted link has none.
          const auto deadline =
              wait ? context.deadline
                   : std::min(context.deadline, DeadlineAfter(timeout));
          response = McpRpc(server, "tools/call", params,
                            SecondsUntil(deadline), true, deadline);
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
          wait = false;
          if (const json* inputs = JsonObject(*result, "inputRequests")) {
            if (!McpInputResponses(server, *inputs, params["inputResponses"],
                                   wait, note)) {
              return;
            }
          } else if (result->contains("inputRequests")) {
            return;
          }
          if (AbortRequested() || SecondsUntil(context.deadline) <= 0) return;
        }
      })) {
    return ToolCancelled("error: call cancelled by user");
  }
  ToolResult result = McpResultText(server, response, context.call_id);
  AddHint(result, Trim(note));
  return result;
}

}  // namespace uagent
