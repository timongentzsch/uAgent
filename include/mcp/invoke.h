// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MCP_INVOKE_H_
#define UAGENT_INCLUDE_MCP_INVOKE_H_
// One bounded MCP invocation path.

#include <set>
#include <string>

#include "include/core/json.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/mcp/result.h"
#include "include/mcp/rpc.h"
#include "include/mcp/server.h"
#include "include/tools/tool.h"

namespace uagent {

inline json McpCallArguments(const json& arguments, const json& input_schema) {
  json normalized = arguments;
  if (normalized.is_object() && input_schema.is_object()) {
    std::set<std::string> required;
    for (const json& name :
         JsonValue(input_schema, "required", json::array())) {
      if (name.is_string()) required.insert(name.get<std::string>());
    }
    for (auto item = normalized.begin(); item != normalized.end();) {
      if (item.value().is_string() &&
          item.value().get_ref<const std::string&>().empty() &&
          !required.contains(item.key())) {
        item = normalized.erase(item);
      } else {
        ++item;
      }
    }
  }
  return normalized;
}

inline ToolResult McpInvokeRemote(McpServer& server,
                                  const std::string& remote_name,
                                  const json& arguments, int64_t timeout,
                                  const ToolContext& context,
                                  const json& input_schema = json::object()) {
  if (!server.alive) {
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "error: mcp server " + server.name + " has exited" +
                           McpStderrHint(server.name));
  }
  json response;
  if (RunCancellable([&] {
        response =
            McpRpc(server, "tools/call",
                   {{"name", remote_name},
                    {"arguments", McpCallArguments(arguments, input_schema)}},
                   context.RemainingSeconds(timeout), true);
      })) {
    return ToolCancelled("error: call cancelled by user");
  }
  return McpResultText(server, response);
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_INVOKE_H_
