// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MCP_REGISTER_H_
#define UAGENT_INCLUDE_MCP_REGISTER_H_
// Configured server startup, discovery, and lifecycle.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/time.h"
#include "include/mcp/config.h"
#include "include/mcp/discover.h"
#include "include/mcp/rpc.h"
#include "include/mcp/server.h"
#include "include/tools/tool.h"

namespace uagent {

inline bool McpStartConfigured(McpServer& server, const RuntimeConfig& config,
                               int64_t& initialize_id, std::string& error) {
  const json& conf = server.config;
  if (!McpResolveRoots(conf, config.mcp_roots, server.roots, error)) {
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
    return false;
  }
  initialize_id = server.next_id++;
  if (!McpSend(server, initialize_id, "server/discover", json::object())) {
    error = "failed to negotiate protocol";
    server.Shutdown();
    return false;
  }
  return true;
}

// Spawn configured servers, handshake, and append one Tool per server tool.
// Spawns everything first and handshakes second, so slow server boots
// (npx downloads, node startup) overlap instead of adding up.
inline std::string McpRegister(std::vector<Tool>& tools, McpRuntime& runtime,
                               const RuntimeConfig& config,
                               const json& trusted_project = nullptr) {
  json cfg = McpLoadConfig(trusted_project,
                           static_cast<size_t>(config.mcp_config_bytes));
  if (cfg.empty()) return {};
  int64_t timeout = config.mcp_timeout_s;

  struct Boot {
    McpServer* s;
    bool required;
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
    bool required = JsonValue(conf, "required", true);
    int64_t id = -1;
    std::string start_error;
    if (!McpStartConfigured(*srv, config, id, start_error)) {
      McpError(name, start_error);
      runtime.Add(std::move(srv));
      ++spawned;
      if (required) {
        return "required MCP server `" + name + "`: " + start_error;
      }
      continue;
    }
    // Queue the handshake now (the pipe buffers it); reap replies below.
    boots.push_back({srv.get(), required});
    srv->initialize_id = id;
    if (!required) srv->startup = McpStartupState::kInitializing;
    runtime.Add(std::move(srv));
    ++spawned;
  }

  for (auto& b : boots) {
    if (!b.required) continue;
    McpServer& s = *b.s;
    // Per server, like McpRefreshTools: a slow neighbour must not consume the
    // handshake window of a server whose reply is already buffered.
    auto startup_deadline = DeadlineAfter(timeout);
    int64_t remaining = SecondsUntil(startup_deadline);
    if (remaining <= 0) {
      McpError(s.name, "startup deadline exceeded");
      s.Shutdown();
      return "required MCP server `" + s.name + "`: startup deadline exceeded";
    }
    std::string initialize_error;
    if (!McpFinishInitialize(s, s.initialize_id, remaining, initialize_error)) {
      McpError(s.name, initialize_error);
      s.Shutdown();
      return "required MCP server `" + s.name + "`: " + initialize_error;
    }
    s.initialize_id = -1;

    if (!McpLoadServerTools(tools, s, config, startup_deadline)) {
      s.Shutdown();
      return "required MCP server `" + s.name + "`: tools/list failed";
    }
  }

  // Optional servers share one small startup window instead of each adding a
  // full timeout. Anything still booting is discovered at a turn boundary.
  auto optional_deadline = DeadlineAfter(config.mcp_startup_grace_s);
  for (;;) {
    bool pending = false;
    for (auto& b : boots) {
      if (b.required || !b.s->alive ||
          b.s->startup == McpStartupState::kReady) {
        continue;
      }
      pending = true;
      McpAdvanceStartup(tools, *b.s, config);
    }
    if (!pending || std::chrono::steady_clock::now() >= optional_deadline) {
      break;
    }
    (void)poll(nullptr, 0, std::min(10, PollTimeoutMs(optional_deadline)));
  }
  for (auto& b : boots) {
    if (!b.required && b.s->alive && b.s->startup != McpStartupState::kReady) {
      McpNote(b.s->name, "starting in background");
    }
  }
  return {};
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_REGISTER_H_
