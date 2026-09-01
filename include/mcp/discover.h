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
#include "include/core/time.h"
#include "include/mcp/invoke.h"
#include "include/mcp/result.h"
#include "include/mcp/rpc.h"
#include "include/mcp/server.h"
#include "include/tools/tool.h"

namespace uagent {

inline bool McpToolPageAllowed(const std::string& name, int64_t max_pages,
                               int64_t& pages, std::set<std::string>& cursors,
                               const std::string& cursor) {
  if (++pages <= max_pages &&
      (cursor.empty() || cursors.insert(cursor).second)) {
    return true;
  }
  McpError(name, "tools/list exceeded pagination limits");
  return false;
}

inline bool McpConsumeToolPage(McpServer& server, const RuntimeConfig& config,
                               const json& response, json& listed,
                               std::string& cursor) {
  if (!response.contains("result") || !response["result"].is_object()) {
    McpError(server.name,
             "tools/list: " + JsonErrorMessage(response, "failed"));
    return false;
  }
  json page = JsonValue(response["result"], "tools", json::array());
  if (!page.is_array()) {
    McpError(server.name, "tools/list returned a non-array `tools` value");
    return false;
  }
  for (const json& definition : page) {
    if (static_cast<int64_t>(listed.size()) >= config.mcp_tools) {
      McpError(server.name, "tool count limit exceeded");
      return false;
    }
    listed.push_back(definition);
  }
  const json& result = response["result"];
  if (result.contains("nextCursor") && !result["nextCursor"].is_string()) {
    McpError(server.name,
             "tools/list returned a non-string `nextCursor` value");
    return false;
  }
  cursor = JsonValue(result, "nextCursor", "");
  return true;
}

inline bool McpFetchToolDefinitions(
    McpServer& s, const RuntimeConfig& config,
    std::chrono::steady_clock::time_point deadline, json& listed) {
  listed = json::array();
  std::string cursor;
  std::set<std::string> cursors;
  int64_t pages = 0;
  do {
    if (!McpToolPageAllowed(s.name, config.mcp_pages, pages, cursors, cursor)) {
      return false;
    }
    int64_t remaining = SecondsUntil(deadline);
    if (remaining <= 0) {
      McpError(s.name, "tools/list deadline exceeded");
      return false;
    }
    json params = cursor.empty() ? json::object() : json{{"cursor", cursor}};
    json resp = McpRpc(s, "tools/list", params, remaining);
    if (!McpConsumeToolPage(s, config, resp, listed, cursor)) return false;
  } while (!cursor.empty());
  return true;
}

// Build a complete replacement before touching the shared registry. A failed
// refresh therefore leaves every previously usable tool in place.
inline void McpReplaceServerTools(std::vector<Tool>& tools, McpServer& s,
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

    std::string task_support = JsonValue(
        JsonValue(definition, "execution", json::object()), "taskSupport", "");
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
    json invocation_schema = input_schema;

    std::string tool_name = McpToolName(s.name, remote_name);
    if (tool_name.empty() || occupied.contains(tool_name)) {
      McpNote(s.name, "duplicate tool name " + tool_name + " skipped");
      continue;
    }
    occupied.insert(tool_name);
    std::string description = JsonValue(definition, "description", "");
    if (description.empty()) description = JsonValue(definition, "title", "");
    if (description.empty()) description = remote_name;
    McpServer* server = &s;
    int64_t call_timeout = config.mcp_timeout_s;
    Tool tool = MakeTool(
        std::move(tool_name), McpCapDesc(description), std::move(input_schema),
        [server, remote_name, call_timeout,
         input_schema = std::move(invocation_schema)](
            const json& arguments, const ToolContext& context) -> ToolResult {
          return McpInvokeRemote(*server, remote_name, arguments, call_timeout,
                                 context, input_schema);
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

    bool read_only =
        JsonValue(JsonValue(definition, "annotations", json::object()),
                  "readOnlyHint", false);
    tool.mutating = !(trust && read_only);
    tool.capabilities = Capability(ToolCapability::kExternal) |
                        Capability(read_only ? ToolCapability::kInspect
                                             : ToolCapability::kMutate);
    schema_bytes += tool_schema_bytes;
    replacement.push_back(std::move(tool));
  }

  std::erase_if(tools,
                [&](const Tool& tool) { return tool.provider == provider; });
  tools.insert(tools.end(), std::make_move_iterator(replacement.begin()),
               std::make_move_iterator(replacement.end()));
  McpNote(s.name, std::to_string(replacement.size()) + " of " +
                      std::to_string(listed.size()) + " tools (~" +
                      FmtCount(static_cast<int64_t>(schema_bytes / 4)) +
                      " schema tokens/request)");
}

inline bool McpLoadServerTools(std::vector<Tool>& tools, McpServer& server,
                               const RuntimeConfig& config,
                               std::chrono::steady_clock::time_point deadline) {
  json listed;
  if (!McpFetchToolDefinitions(server, config, deadline, listed)) return false;
  McpReplaceServerTools(tools, server, config, listed);
  return true;
}

inline bool McpSendStartupToolPage(McpServer& server,
                                   const RuntimeConfig& config) {
  if (!McpToolPageAllowed(server.name, config.mcp_pages, server.startup_pages,
                          server.startup_cursors, server.startup_cursor)) {
    server.Shutdown();
    return false;
  }
  json params = server.startup_cursor.empty()
                    ? json::object()
                    : json{{"cursor", server.startup_cursor}};
  server.tools_list_id = server.next_id++;
  if (McpSend(server, server.tools_list_id, "tools/list", params)) return true;
  server.Shutdown();
  return false;
}

// Keep one tools/list request outstanding across short startup windows. A
// healthy slow optional server neither blocks startup nor loses its eventual
// reply to a newer request ID.
inline bool McpAdvanceStartupTools(std::vector<Tool>& tools, McpServer& server,
                                   const RuntimeConfig& config) {
  for (;;) {
    if (server.tools_list_id < 0 && !McpSendStartupToolPage(server, config)) {
      return false;
    }
    json response;
    McpResponseState state =
        McpTryResponse(server, server.tools_list_id, response);
    if (state == McpResponseState::kPending) return false;
    if (state == McpResponseState::kClosed) {
      McpError(server.name,
               "tools/list: server exited" + McpStderrHint(server.name));
      server.Shutdown();
      return false;
    }
    if (!response.contains("result") || !response["result"].is_object()) {
      McpError(server.name,
               "tools/list: " + JsonErrorMessage(response, "failed"));
      server.Shutdown();
      return false;
    }
    server.tools_list_id = -1;
    if (!McpConsumeToolPage(server, config, response, server.startup_tools,
                            server.startup_cursor)) {
      server.Shutdown();
      return false;
    }
    if (!server.startup_cursor.empty()) continue;
    McpReplaceServerTools(tools, server, config, server.startup_tools);
    server.startup_tools = json::array();
    server.startup = McpStartupState::kReady;
    return true;
  }
}

// Advance one optional server without consuming its discovery reply in the
// idle-notification drain. A failed aggregate startup deadline leaves the
// state pending; the next turn resumes the same request for another bounded
// opportunity.
inline bool McpAdvanceStartup(std::vector<Tool>& tools, McpServer& server,
                              const RuntimeConfig& config) {
  if (server.startup == McpStartupState::kInitializing) {
    json discovery;
    McpResponseState state =
        McpTryResponse(server, server.initialize_id, discovery);
    if (state == McpResponseState::kPending) return false;
    std::string error;
    if (state == McpResponseState::kClosed) {
      error = "server exited" + McpStderrHint(server.name);
    } else if (McpValidateDiscovery(discovery, error)) {
      server.initialize_id = -1;
      server.startup = McpStartupState::kDiscoveringTools;
    }
    if (!error.empty()) {
      McpError(server.name, error);
      server.Shutdown();
      return false;
    }
  }
  if (server.startup != McpStartupState::kDiscoveringTools) {
    return false;
  }
  return McpAdvanceStartupTools(tools, server, config);
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
    if (server.alive && server.startup != McpStartupState::kReady) {
      if (McpAdvanceStartup(tools, server, config)) {
        changed = true;
        McpNote(server.name, "optional server ready");
      }
      continue;
    }
    if (server.alive) McpDrainInbound(server);
    if (!server.alive || !server.tools_changed) continue;
    auto deadline = DeadlineAfter(config.mcp_timeout_s);
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
