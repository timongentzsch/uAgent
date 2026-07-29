// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MCP_DISCOVER_H_
#define UAGENT_INCLUDE_MCP_DISCOVER_H_
// Tool discovery: fetch a server's definitions and fold them into the
// registry, replacing a previous generation in place.

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/mcp/result.h"
#include "include/mcp/rpc.h"
#include "include/mcp/server.h"
#include "include/tools/tool.h"

namespace uagent {

inline bool McpFetchToolDefinitions(
    McpServer& s, const RuntimeConfig& config,
    std::chrono::steady_clock::time_point deadline, json& listed) {
  listed = json::array();
  std::string cursor;
  std::set<std::string> cursors;
  int64_t pages = 0;
  do {
    if (++pages > config.mcp_pages ||
        (!cursor.empty() && !cursors.insert(cursor).second)) {
      McpError(s.name, "tools/list exceeded pagination limits");
      return false;
    }
    int64_t remaining = std::chrono::duration_cast<std::chrono::seconds>(
                            deadline - std::chrono::steady_clock::now())
                            .count();
    if (remaining <= 0) {
      McpError(s.name, "tools/list deadline exceeded");
      return false;
    }
    json params = cursor.empty() ? json::object() : json{{"cursor", cursor}};
    json resp = McpRpc(s, "tools/list", params, remaining);
    if (!resp.contains("result") || !resp["result"].is_object()) {
      std::string message = "failed";
      if (resp.contains("error") && resp["error"].is_object() &&
          resp["error"].contains("message") &&
          resp["error"]["message"].is_string()) {
        message = resp["error"]["message"].get<std::string>();
      }
      McpError(s.name, "tools/list: " + message);
      return false;
    }
    json page = JsonValue(resp["result"], "tools", json::array());
    if (!page.is_array()) {
      McpError(s.name, "tools/list returned a non-array `tools` value");
      return false;
    }
    for (const json& definition : page) {
      if (static_cast<int64_t>(listed.size()) >= config.mcp_tools) {
        McpError(s.name, "tool count limit exceeded");
        return false;
      }
      listed.push_back(definition);
    }
    const json& result = resp["result"];
    if (result.contains("nextCursor") && !result["nextCursor"].is_string()) {
      McpError(s.name, "tools/list returned a non-string `nextCursor`");
      return false;
    }
    cursor = JsonValue(result, "nextCursor", "");
  } while (!cursor.empty());
  return true;
}

// Build a complete replacement before touching the shared registry. A failed
// refresh therefore leaves every previously usable tool in place.
inline bool McpReplaceServerTools(std::vector<Tool>& tools, McpServer& s,
                                  const RuntimeConfig& config,
                                  const json& listed) {
  const std::string provider = "mcp:" + s.name;
  std::set<std::string> occupied;
  for (const Tool& tool : tools) {
    if (tool.provider != provider) occupied.insert(tool.name);
  }

  std::set<std::string> only;
  if (s.config.contains("tools")) {
    for (const json& name : s.config["tools"]) {
      only.insert(name.get<std::string>());
    }
  }
  const bool trust = JsonValue(s.config, "trust", false);

  std::vector<Tool> replacement;
  size_t schema_bytes = 0;
  size_t max_schema_bytes = static_cast<size_t>(config.mcp_schema_bytes);
  for (const json& definition : listed) {
    if (!definition.is_object() || !definition.contains("name") ||
        !definition["name"].is_string()) {
      McpNote(s.name, "invalid tool definition skipped");
      continue;
    }
    const std::string remote_name = definition["name"].get<std::string>();
    if (remote_name.empty() || (!only.empty() && !only.contains(remote_name))) {
      continue;
    }

    std::string task_support;
    if (definition.contains("execution") &&
        definition["execution"].is_object() &&
        definition["execution"].contains("taskSupport") &&
        definition["execution"]["taskSupport"].is_string()) {
      task_support = definition["execution"]["taskSupport"].get<std::string>();
    }
    if (task_support == "required") {
      McpNote(s.name, remote_name + " skipped (requires MCP tasks)");
      continue;
    }

    json input_schema =
        JsonValue(definition, "inputSchema", json{{"type", "object"}});
    if (!input_schema.is_object()) {
      McpNote(s.name, remote_name + " skipped (inputSchema is not an object)");
      continue;
    }

    std::string tool_name = McpToolName(s.name, remote_name);
    if (tool_name.empty() || occupied.contains(tool_name)) {
      McpNote(s.name, "duplicate tool name " + tool_name + " skipped");
      continue;
    }
    occupied.insert(tool_name);
    std::string description;
    if (definition.contains("description") &&
        definition["description"].is_string()) {
      description = definition["description"].get<std::string>();
    }
    if (description.empty() && definition.contains("title") &&
        definition["title"].is_string()) {
      description = definition["title"].get<std::string>();
    }
    if (description.empty()) description = remote_name;
    McpServer* server = &s;
    int64_t call_timeout = config.mcp_timeout_s;
    Tool tool = MakeTool(
        std::move(tool_name), McpCapDesc(description), std::move(input_schema),
        [server, remote_name, call_timeout](
            const json& arguments, const ToolContext& context) -> ToolResult {
          if (!server->alive) {
            return ToolFailure(
                ToolErrorCode::kUnavailable,
                "error: mcp server " + server->name +
                    " has exited (stderr: " + McpLogPath(server->name) + ")");
          }
          json response;
          if (RunCancellable([&] {
                response =
                    McpRpc(*server, "tools/call",
                           {{"name", remote_name}, {"arguments", arguments}},
                           context.RemainingSeconds(call_timeout), true);
              })) {
            return ToolCancelled("error: call cancelled by user");
          }
          return McpResultText(*server, response);
        });
    if (definition.contains("outputSchema") &&
        definition["outputSchema"].is_object()) {
      tool.output_schema = definition["outputSchema"];
    }
    tool.provider = provider;
    tool.timeout_s = config.mcp_timeout_s;

    size_t tool_schema_bytes =
        tool.description.size() + JsonDump(tool.parameters).size() +
        (tool.output_schema.is_null() ? 0
                                      : JsonDump(tool.output_schema).size());
    if (schema_bytes + tool_schema_bytes > max_schema_bytes) {
      McpNote(s.name, "remaining tools skipped (schema byte limit)");
      break;
    }

    bool read_only = definition.contains("annotations") &&
                     definition["annotations"].is_object() &&
                     definition["annotations"].contains("readOnlyHint") &&
                     definition["annotations"]["readOnlyHint"].is_boolean() &&
                     definition["annotations"]["readOnlyHint"].get<bool>();
    tool.mutating = !(trust && read_only);
    schema_bytes += tool_schema_bytes;
    replacement.push_back(std::move(tool));
  }

  std::erase_if(tools,
                [&](const Tool& tool) { return tool.provider == provider; });
  tools.insert(tools.end(), std::make_move_iterator(replacement.begin()),
               std::make_move_iterator(replacement.end()));
  McpNote(s.name, std::to_string(replacement.size()) + " of " +
                      std::to_string(listed.size()) + " tools (~" +
                      FmtTokens(static_cast<int64_t>(schema_bytes / 4)) +
                      " schema tokens/request)");
  return true;
}

inline bool McpLoadServerTools(std::vector<Tool>& tools, McpServer& server,
                               const RuntimeConfig& config,
                               std::chrono::steady_clock::time_point deadline) {
  json listed;
  return McpFetchToolDefinitions(server, config, deadline, listed) &&
         McpReplaceServerTools(tools, server, config, listed);
}

// Apply notifications only between tool batches, when the agent holds no Tool
// pointers. Failed refreshes retain the prior registry and retry later.
inline bool McpRefreshTools(
    std::vector<Tool>& tools, McpRuntime& runtime, const RuntimeConfig& config,
    std::chrono::steady_clock::time_point turn_deadline =
        std::chrono::steady_clock::time_point::max()) {
  bool changed = false;
  for (const auto& owned : runtime.Servers()) {
    McpServer& server = *owned;
    if (server.alive) McpDrainInbound(server);
    if (!server.alive || !server.tools_changed) continue;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(config.mcp_timeout_s);
    if (turn_deadline != std::chrono::steady_clock::time_point::max()) {
      deadline = std::min(deadline, turn_deadline);
    }
    if (McpLoadServerTools(tools, server, config, deadline)) {
      server.tools_changed = false;
      changed = true;
      McpNote(server.name, "tool registry refreshed");
    }
  }
  return changed;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_DISCOVER_H_
