// Copyright 2026 Timon Gentzsch

#include "include/mcp/discover.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/agent/activity_supervisor.h"
#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/runtime_config.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/mcp/invoke.h"
#include "include/mcp/result.h"
#include "include/mcp/rpc.h"
#include "include/mcp/server.h"
#include "include/mcp/skills.h"
#include "include/tools/tool.h"

namespace uagent {

bool McpPageAllowed(const std::string& name, const char* method, int64_t& pages,
                    std::set<std::string>& cursors, const std::string& cursor) {
  if (++pages <= kMcpPages &&
      (cursor.empty() || cursors.insert(cursor).second)) {
    return true;
  }
  McpError(name, std::string(method) + " exceeded pagination limits");
  return false;
}

bool McpConsumePage(McpServer& server, const char* method, const json& response,
                    json& listed, std::string& cursor) {
  if (!response.contains("result") || !response["result"].is_object()) {
    McpError(server.name,
             std::string(method) + ": " + JsonErrorMessage(response, "failed"));
    return false;
  }
  // "tools" for tools/list: the key its pages carry.
  const std::string key(method, std::string_view(method).find('/'));
  json page = JsonValue(response["result"], key.c_str(), json::array());
  if (!page.is_array()) {
    McpError(server.name,
             std::string(method) + " returned a non-array `" + key + "` value");
    return false;
  }
  for (const json& definition : page) {
    if (listed.size() >= MaxMcpTools()) {
      McpError(server.name, key + " count limit exceeded");
      return false;
    }
    listed.push_back(definition);
  }
  const json& result = response["result"];
  if (result.contains("nextCursor") && !result["nextCursor"].is_string()) {
    McpError(server.name,
             std::string(method) + " returned a non-string `nextCursor` value");
    return false;
  }
  cursor = JsonValue(result, "nextCursor", "");
  return true;
}

bool McpFetchList(McpServer& s, const char* method,
                  std::chrono::steady_clock::time_point deadline,
                  json& listed) {
  listed = json::array();
  std::string cursor;
  std::set<std::string> cursors;
  int64_t pages = 0;
  do {
    if (!McpPageAllowed(s.name, method, pages, cursors, cursor)) {
      return false;
    }
    int64_t remaining = SecondsUntil(deadline);
    if (remaining <= 0) {
      McpError(s.name, std::string(method) + " deadline exceeded");
      return false;
    }
    json params = cursor.empty() ? json::object() : json{{"cursor", cursor}};
    json resp = McpRpc(s, method, params, remaining, false, deadline);
    if (!McpConsumePage(s, method, resp, listed, cursor)) return false;
  } while (!cursor.empty());
  return true;
}

void McpReplaceServerTools(std::vector<Tool>& tools, McpServer& s,
                           const RuntimeConfig& config, const json& listed) {
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
    std::string description = JsonValue(definition, "description", "");
    if (description.empty()) description = JsonValue(definition, "title", "");
    if (description.empty()) description = remote_name;
    McpServer* server = &s;
    int64_t call_timeout = config.mcp_timeout_s;
    Tool tool = MakeTool(
        std::move(tool_name), McpCapDesc(description), std::move(input_schema),
        [server, remote_name, call_timeout](
            const json& arguments, const ToolContext& context) -> ToolResult {
          return McpInvokeRemote(*server, remote_name, arguments, call_timeout,
                                 context);
        });
    tool.provider = provider;
    // Keep the server and its original tool name legible after namespacing.
    tool.headline = [label = s.name + "/" + remote_name](const json& args) {
      std::string target;
      for (const char* key : {"query", "path", "url", "name", "title", "id"}) {
        if (const auto* value = JsonStringRef(args, key);
            value && !value->empty()) {
          target = OneLine(*value, 160);
          break;
        }
      }
      return Headline{"Calling " + label, "Called " + label, std::move(target)};
    };
    // McpInvokeRemote bounds each request: a person filling in the server's
    // form is not the server's time.
    tool.timeout_s = 0;

    size_t tool_schema_bytes =
        tool.description.size() + JsonDump(tool.parameters).size();
    if (schema_bytes + tool_schema_bytes > kMcpSchemaBytes) {
      McpNote(s.name, "remaining tools skipped (schema byte limit)");
      break;
    }

    bool read_only =
        trust && JsonValue(JsonValue(definition, "annotations", json::object()),
                           "readOnlyHint", false);
    tool.mutating = !read_only;
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
  if (s.alive && s.subscription_id < 0) {
    s.subscription_id = s.next_id++;
    McpSend(s, s.subscription_id, "subscriptions/listen",
            {{"notifications", {{"toolsListChanged", true}}}});
  }
  McpNote(s.name, std::to_string(replacement.size()) + " of " +
                      std::to_string(listed.size()) + " tools (" +
                      FmtCount(static_cast<int64_t>(schema_bytes)) +
                      " serialized schema bytes/request)");
}

bool McpLoadServerTools(std::vector<Tool>& tools, McpServer& server,
                        const RuntimeConfig& config,
                        std::chrono::steady_clock::time_point deadline) {
  json listed;
  if (!McpFetchList(server, "tools/list", deadline, listed)) return false;
  McpReplaceServerTools(tools, server, config, listed);
  return true;
}

namespace {
// Its tools are offered only once its skills are in too: a tool call while a
// list is outstanding would read past that list's reply.
void FinishStartupList(std::vector<Tool>& tools, McpServer& server,
                       const RuntimeConfig& config) {
  json listed = std::exchange(server.startup_listed, json::array());
  server.startup_pages = 0;
  server.startup_cursors.clear();
  if (server.startup == McpStartupState::kDiscoveringTools) {
    server.startup_tools = std::move(listed);
    if (server.serves_skills) {
      server.startup = McpStartupState::kDiscoveringSkills;
      return;
    }
  } else {
    McpSetSkills(server, listed);
  }
  if (server.serves_tools) {
    McpReplaceServerTools(tools, server, config,
                          std::exchange(server.startup_tools, json::array()));
  }
  server.startup = McpStartupState::kReady;
}
}  // namespace

bool McpAdvanceStartupLists(std::vector<Tool>& tools, McpServer& server,
                            const RuntimeConfig& config) {
  while (server.startup == McpStartupState::kDiscoveringTools ||
         server.startup == McpStartupState::kDiscoveringSkills) {
    const bool skills = server.startup == McpStartupState::kDiscoveringSkills;
    const char* method = skills ? "skills/list" : "tools/list";
    bool failed = false;
    if (server.list_id < 0) {
      failed = !McpPageAllowed(server.name, method, server.startup_pages,
                               server.startup_cursors, server.startup_cursor);
      if (!failed) {
        json params = server.startup_cursor.empty()
                          ? json::object()
                          : json{{"cursor", server.startup_cursor}};
        server.list_id = server.next_id++;
        if (!McpSend(server, server.list_id, method, params)) {
          server.Shutdown();
          return false;
        }
      }
    }
    if (!failed) {
      json response;
      McpResponseState state = McpTryResponse(server, server.list_id, response);
      if (state == McpResponseState::kPending) return false;
      if (state == McpResponseState::kClosed) {
        server.error = std::string(method) + ": server exited";
        McpError(server.name, server.error + McpStderrHint(server.name));
        server.Shutdown();
        return false;
      }
      server.list_id = -1;
      failed = !McpConsumePage(server, method, response, server.startup_listed,
                               server.startup_cursor);
    }
    // Skills that cannot be listed leave the server with its tools alone.
    if (failed && !skills) {
      server.Shutdown();
      return false;
    }
    if (failed) {
      server.startup_listed = json::array();
      server.startup_cursor.clear();
    }
    if (server.startup_cursor.empty()) FinishStartupList(tools, server, config);
  }
  return true;
}

bool McpAdvanceStartup(std::vector<Tool>& tools, McpServer& server,
                       const RuntimeConfig& config) {
  if (server.startup == McpStartupState::kDiscovering) {
    json discovery;
    McpResponseState state =
        McpTryResponse(server, server.discovery_id, discovery);
    if (state == McpResponseState::kPending) return false;
    std::string error;
    if (state == McpResponseState::kClosed) {
      error = "server exited";
    } else if (McpValidateDiscovery(server, discovery, error)) {
      server.discovery_id = -1;
      server.startup = server.serves_tools
                           ? McpStartupState::kDiscoveringTools
                           : McpStartupState::kDiscoveringSkills;
    }
    if (!error.empty()) {
      McpError(server.name, state == McpResponseState::kClosed
                                ? error + McpStderrHint(server.name)
                                : error);
      server.error = error;
      server.Shutdown();
      return false;
    }
  }
  if (server.startup == McpStartupState::kDiscovering) return false;
  return McpAdvanceStartupLists(tools, server, config);
}

bool McpRefreshTools(std::vector<Tool>& tools, McpRuntime& runtime,
                     const RuntimeConfig& config,
                     std::chrono::steady_clock::time_point turn_deadline) {
  if (runtime.processes) runtime.processes->Tick(true);
  bool changed = std::exchange(runtime.registry_changed, false);
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
    // A server that has gone takes its tools out of what is offered.
    if (!server.alive) {
      const std::string provider = "mcp:" + server.name;
      changed |= std::erase_if(tools, [&](const Tool& tool) {
                   return tool.provider == provider;
                 }) > 0;
      continue;
    }
    if (!server.tools_changed) continue;
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
