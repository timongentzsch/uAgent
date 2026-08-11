// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MCP_REGISTER_H_
#define UAGENT_INCLUDE_MCP_REGISTER_H_
// Server lifecycle and registration: start, initialize, restart, the
// Chrome session tool, and the one entry point the REPL calls.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/time.h"
#include "include/mcp/config.h"
#include "include/mcp/discover.h"
#include "include/mcp/invoke.h"
#include "include/mcp/rpc.h"
#include "include/mcp/server.h"
#include "include/tools/tool.h"

namespace uagent {

inline bool McpStartConfigured(McpServer& server, const RuntimeConfig& config,
                               int64_t& initialize_id, std::string& error) {
  const json& conf = server.config;
  if (!McpResolveRoots(conf, config.mcp_roots, server.roots, error)) {
    server.last_error = error;
    return false;
  }
  std::vector<std::string> args;
  if (conf.contains("args")) {
    for (const json& value : conf["args"]) {
      args.push_back(value.get<std::string>());
    }
  }
  std::vector<std::pair<std::string, std::string>> env;
  if (conf.contains("env")) {
    for (const auto& [key, value] : conf["env"].items()) {
      env.emplace_back(key, ExpandProcessEnv(value.get<std::string>()));
    }
  }

  std::filesystem::path cwd = JsonValue(conf, "cwd", "");
  if (!cwd.empty() && cwd.is_relative()) {
    cwd =
        std::filesystem::path(JsonValue(conf, "__uagent_config_dir", "")) / cwd;
  }
  if (!cwd.empty()) {
    std::error_code ec;
    cwd = std::filesystem::weakly_canonical(cwd, ec);
    if (ec || !std::filesystem::is_directory(cwd, ec)) {
      error = "invalid cwd `" + cwd.string() + "`";
      return false;
    }
  }
  if (!McpSpawn(server, conf["command"].get<std::string>(), args, env,
                cwd.string(), static_cast<size_t>(config.mcp_log_bytes))) {
    error = "failed to start";
    server.last_error = error;
    return false;
  }
  initialize_id = server.next_id++;
  if (!McpSend(server, initialize_id, "initialize",
               {{"protocolVersion", "2025-11-25"},
                {"capabilities", {{"roots", {{"listChanged", false}}}}},
                {"clientInfo", {{"name", "uagent"}, {"version", kVersion}}}})) {
    error = "failed to initialize";
    server.last_error = error;
    server.Shutdown();
    return false;
  }
  return true;
}

inline bool McpFinishInitialize(McpServer& server, int64_t initialize_id,
                                int64_t timeout, std::string& error) {
  json init = McpAwait(server, initialize_id, timeout, false);
  if (!init.contains("result")) {
    error = "handshake failed";
    if (init.contains("error") && init["error"].is_object() &&
        init["error"].contains("message") &&
        init["error"]["message"].is_string()) {
      error = init["error"]["message"].get<std::string>();
    }
    server.last_error = error;
    return false;
  }
  const json& result = init["result"];
  std::string protocol = JsonValue(result, "protocolVersion", "");
  if (protocol != "2025-11-25" && protocol != "2025-06-18") {
    error = "unsupported protocol version `" + protocol + "`";
    server.last_error = error;
    return false;
  }
  if (!result.contains("capabilities") || !result["capabilities"].is_object() ||
      !result["capabilities"].contains("tools")) {
    error = "server did not negotiate tools capability";
    server.last_error = error;
    return false;
  }
  McpSend(server, -1, "notifications/initialized", json::object());
  server.last_error.clear();
  return true;
}

inline bool McpRestart(McpServer& server, const json& next_config,
                       const RuntimeConfig& config, int64_t timeout,
                       std::string& error) {
  json previous = server.config;
  bool was_alive = server.alive;
  server.Shutdown();
  server.config = next_config;
  int64_t initialize_id = -1;
  if (McpStartConfigured(server, config, initialize_id, error) &&
      McpFinishInitialize(server, initialize_id, timeout, error)) {
    server.tools_changed = true;
    server.last_error.clear();
    return true;
  }

  server.Shutdown();
  if (!was_alive) {
    server.last_error = error;
    return false;
  }
  server.config = std::move(previous);
  std::string restore_error;
  if (McpStartConfigured(server, config, initialize_id, restore_error) &&
      McpFinishInitialize(server, initialize_id, timeout, restore_error)) {
    return false;
  }
  server.Shutdown();
  error += "; previous session could not be restored: " + restore_error;
  server.last_error = error;
  return false;
}

inline McpServer* McpFindServer(McpRuntime& runtime, std::string_view name) {
  for (const auto& server : runtime.Servers()) {
    if (server->name == name) return server.get();
  }
  return nullptr;
}

inline void McpAddControlTools(std::vector<Tool>& tools, McpRuntime& runtime,
                               const RuntimeConfig& config) {
  Tool& status = AddTool(
      tools,
      MakeTool(
          "mcp_status",
          "Show lifecycle state, process id, discovered tool count, and last "
          "error for all configured MCP servers or one named server.",
          {{"type", "object"},
           {"properties", {{"server", {{"type", "string"}}}}},
           {"additionalProperties", false}},
          [&runtime](const json& args, const ToolContext&) -> ToolResult {
            std::string wanted = JsonValue(args, "server", "");
            std::string output;
            for (const auto& owned : runtime.Servers()) {
              const McpServer& server = *owned;
              if (!wanted.empty() && server.name != wanted) continue;
              std::string state =
                  !server.last_error.empty()
                      ? "error"
                      : (server.alive
                             ? "ready"
                             : (McpConfigLazy(server.config) ? "inactive"
                                                             : "stopped"));
              if (!output.empty()) output += '\n';
              output += server.name + " · " + state + " · pid " +
                        std::to_string(server.pid) + " · tools " +
                        std::to_string(server.tool_count);
              std::string mode = JsonValue(server.config, "__uagent_mode", "");
              if (!mode.empty()) {
                output += " · " + mode + "/" +
                          JsonValue(server.config, "__uagent_toolset", "slim");
              }
              if (!server.last_error.empty()) {
                output += " · " + FirstLine(server.last_error);
              }
            }
            if (output.empty()) {
              return wanted.empty()
                         ? ToolSuccess("(no MCP servers configured)")
                         : ToolFailure(ToolErrorCode::kNotFound,
                                       "error: no configured MCP server: " +
                                           TerminalSafe(wanted));
            }
            return ToolSuccess(std::move(output));
          }));
  status.parallel_safe = true;
  status.capabilities = Capability(ToolCapability::kInspect) |
                        Capability(ToolCapability::kExternal);
  status.provider = "builtin:mcp";
  status.summary = [](const json& args) {
    return JsonValue(args, "server", "all");
  };

  Tool& restart = AddTool(
      tools,
      MakeTool(
          "mcp_restart",
          "Restart and handshake one configured MCP server. Its tools refresh "
          "at the next model step; use chrome_session for Chrome mode changes "
          "and page health.",
          {{"type", "object"},
           {"properties", {{"server", {{"type", "string"}}}}},
           {"required", json::array({"server"})},
           {"additionalProperties", false}},
          [&runtime, &config](const json& args,
                              const ToolContext& context) -> ToolResult {
            std::string name = JsonValue(args, "server", "");
            McpServer* server = McpFindServer(runtime, name);
            if (!server) {
              return ToolFailure(
                  ToolErrorCode::kNotFound,
                  "error: no configured MCP server: " + TerminalSafe(name));
            }
            std::string error;
            if (!McpRestart(*server, server->config, config,
                            context.RemainingSeconds(config.mcp_timeout_s),
                            error)) {
              return ToolFailure(ToolErrorCode::kUnavailable,
                                 "error: could not restart MCP server " +
                                     TerminalSafe(name) + ": " + error);
            }
            return ToolSuccess("MCP server " + name +
                               " restarted; handshake passed and tools will "
                               "refresh next step");
          }));
  restart.mutating = true;
  restart.capabilities = Capability(ToolCapability::kExecute) |
                         Capability(ToolCapability::kMutate) |
                         Capability(ToolCapability::kExternal);
  restart.provider = "builtin:mcp";
  restart.summary = [](const json& args) {
    return JsonValue(args, "server", "");
  };
}

inline void McpAddChromeSessionTool(std::vector<Tool>& tools,
                                    McpRuntime& runtime,
                                    const RuntimeConfig& config) {
  McpServer* chrome = nullptr;
  for (const auto& server : runtime.Servers()) {
    if (JsonValue(server->config, "__uagent_builtin", "") == kChromeMcpName) {
      chrome = server.get();
      break;
    }
  }
  if (!chrome) return;

  Tool& tool = AddTool(
      tools,
      MakeTool(
          "chrome_session",
          "Start/switch Chrome MCP. slim (default) provides navigate, "
          "evaluate, screenshot; full adds granular UI, network, console "
          "and performance tools.",
          {{"type", "object"},
           {"properties",
            {{"mode",
              {{"type", "string"},
               {"enum", json::array({"isolated", "user"})}}},
             {"toolset",
              {{"type", "string"},
               {"enum", json::array({"slim", "full"})},
               {"description", "default slim"}}}}},
           {"required", json::array({"mode"})},
           {"additionalProperties", false}},
          [chrome, &config](const json& args,
                            const ToolContext& context) -> ToolResult {
            std::string mode = JsonValue(args, "mode", "");
            std::string toolset = JsonValue(args, "toolset", "slim");
            if (mode != "isolated" && mode != "user") {
              return ToolFailure(ToolErrorCode::kInvalidArguments,
                                 "error: mode must be isolated or user");
            }
            if (toolset != "slim" && toolset != "full") {
              return ToolFailure(ToolErrorCode::kInvalidArguments,
                                 "error: toolset must be slim or full");
            }
            bool already_selected =
                chrome->alive &&
                JsonValue(chrome->config, "__uagent_mode", "isolated") ==
                    mode &&
                JsonValue(chrome->config, "__uagent_toolset", "slim") ==
                    toolset;
            if (!already_selected) {
              json next = ChromeMcpConfig(mode, toolset);
              std::string error;
              if (!McpRestart(*chrome, next, config,
                              context.RemainingSeconds(config.mcp_timeout_s),
                              error)) {
                return ToolFailure(
                    ToolErrorCode::kUnavailable,
                    "error: could not switch Chrome DevTools: " + error);
              }
            }
            bool slim = toolset == "slim";
            const char* probe = slim ? "evaluate" : "evaluate_script";
            json probe_args =
                slim ? json{{"script", "document.location.href"}}
                     : json{{"function", "() => document.location.href"}};
            ToolResult health = McpInvokeRemote(*chrome, probe, probe_args,
                                                config.mcp_timeout_s, context,
                                                /*include_snapshot=*/false);
            if (health.status == CompletionStatus::kCancelled) {
              return health;
            }
            if (!health.Ok() || health.output == "(empty result)") {
              std::string reason = std::move(health.output);
              if (reason.starts_with("error: ")) reason.erase(0, 7);
              std::string hint =
                  mode == "user"
                      ? " Open a normal page in the shared Chrome window "
                        "and retry."
                      : " Retry the isolated session.";
              if (slim) {
                hint +=
                    " Use toolset=full if the slim bridge still "
                    "cannot expose the upstream page error.";
              }
              chrome->last_error = "page health failed: " + reason;
              return ToolFailure(
                  ToolErrorCode::kUnavailable,
                  "error: Chrome DevTools " + mode + "/" + toolset +
                      " session has no usable page: " + reason + "." + hint);
            }
            chrome->last_error.clear();
            if (already_selected) {
              return ToolSuccess("Chrome DevTools is already using " + mode +
                                 "/" + toolset + " (page health check passed)");
            }
            return ToolSuccess((mode == "user" ? "User" : "Isolated") +
                               std::string(" Chrome session selected (") +
                               toolset + "; page health check passed)");
          }));
  tool.mutating = true;
  tool.capabilities = Capability(ToolCapability::kExecute) |
                      Capability(ToolCapability::kMutate) |
                      Capability(ToolCapability::kExternal);
  tool.provider = "builtin:chrome";
  tool.summary = [](const json& args) {
    return JsonValue(args, "mode", "") + "/" +
           JsonValue(args, "toolset", "slim");
  };
}

// Spawn configured servers, handshake, and append one Tool per server tool.
// Spawns everything first and handshakes second, so slow server boots
// (npx downloads, node startup) overlap instead of adding up.
inline void McpRegister(std::vector<Tool>& tools, McpRuntime& runtime,
                        const RuntimeConfig& config,
                        const json& trusted_project = nullptr) {
  json cfg = McpLoadConfig(trusted_project,
                           static_cast<size_t>(config.mcp_config_bytes));
  if (cfg.empty()) return;
  bool custom_server =
      std::any_of(cfg.begin(), cfg.end(), [](const json& server) {
        return JsonValue(server, "__uagent_builtin", "").empty();
      });
  // Chrome already has a narrower mode/readiness control. Avoid paying for
  // generic MCP lifecycle schemas when it is the only configured server.
  if (custom_server) McpAddControlTools(tools, runtime, config);
  int64_t timeout = config.mcp_timeout_s;

  struct Boot {
    McpServer* s;
    int64_t init_id;
  };
  std::vector<Boot> boots;
  // config and server replies are untrusted JSON: a wrong type anywhere
  // must skip that server, never take the agent down
  int64_t max_servers = config.mcp_servers;
  int64_t spawned = 0;
  for (auto& [name, conf] : cfg.items()) {
    if (spawned >= max_servers) {
      McpNote(name, "skipped (server limit reached)");
      continue;
    }
    std::string config_error;
    if (!McpValidateServerConfig(name, conf, config_error)) {
      McpError(name, "invalid config: " + config_error);
      continue;
    }
    if (JsonValue(conf, "disabled", false)) {
      McpNote(name, "disabled");
      continue;
    }
    std::string type = JsonValue(conf, "type", "stdio");
    if (type != "stdio") {
      McpNote(name, "skipped (transport `" + type + "` not supported)");
      continue;
    }
    auto srv = std::make_unique<McpServer>();
    srv->name = name;
    srv->response_cap = static_cast<size_t>(config.mcp_response_bytes);
    srv->config = conf;
    if (McpConfigLazy(conf)) {
      runtime.Add(std::move(srv));  // activate only after chrome_session
      ++spawned;
      continue;
    }
    int64_t id = -1;
    std::string start_error;
    if (!McpStartConfigured(*srv, config, id, start_error)) {
      McpError(name, start_error);
      srv->last_error = start_error;
      runtime.Add(std::move(srv));
      ++spawned;
      continue;
    }
    // Queue the handshake now (the pipe buffers it); reap replies below.
    boots.push_back({srv.get(), id});
    runtime.Add(std::move(srv));
    ++spawned;
  }

  auto startup_deadline = DeadlineAfter(timeout);
  for (auto& b : boots) {
    McpServer& s = *b.s;
    int64_t remaining = std::chrono::duration_cast<std::chrono::seconds>(
                            startup_deadline - std::chrono::steady_clock::now())
                            .count();
    if (remaining <= 0) {
      McpError(s.name, "startup deadline exceeded");
      s.Shutdown();
      continue;
    }
    std::string initialize_error;
    if (!McpFinishInitialize(s, b.init_id, remaining, initialize_error)) {
      McpError(s.name, initialize_error);
      s.Shutdown();
      continue;
    }

    if (!McpLoadServerTools(tools, s, config, startup_deadline)) {
      s.Shutdown();
      continue;
    }
  }
  McpAddChromeSessionTool(tools, runtime, config);
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_REGISTER_H_
