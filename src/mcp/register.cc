// Copyright 2026 Timon Gentzsch

#include "include/mcp/register.h"

#include <poll.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/config.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/time.h"
#include "include/mcp/config.h"
#include "include/mcp/discover.h"
#include "include/mcp/rpc.h"
#include "include/mcp/server.h"
#include "include/tools/tool.h"

namespace uagent {

bool McpStartConfigured(McpServer& server, const RuntimeConfig& config,
                        int64_t& discovery_id, std::string& error) {
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
                cwd.string())) {
    error = "failed to start";
    return false;
  }
  discovery_id = server.next_id++;
  if (!McpSend(server, discovery_id, "server/discover", json::object())) {
    error = "failed to negotiate protocol";
    server.Shutdown();
    return false;
  }
  return true;
}

std::string McpRegister(std::vector<Tool>& tools, McpRuntime& runtime,
                        const RuntimeConfig& config,
                        const json& trusted_project) {
  json cfg = McpLoadConfig(trusted_project);
  if (cfg.empty()) return {};
  int64_t timeout = config.mcp_timeout_s;

  struct Boot {
    McpServer* s;
    bool required;
  };
  std::vector<Boot> boots;
  // config and server replies are untrusted JSON: a wrong type anywhere
  // must skip that server, never take the agent down
  int64_t spawned = 0;
  for (auto& [name, conf] : cfg.items()) {
    auto srv = std::make_unique<McpServer>();
    srv->name = name;
    srv->config = conf.is_object() ? conf : json::object();
    // Every configured server is recorded, running or not, so the settings
    // overview can say why one is missing.
    auto skip = [&](std::string why) {
      srv->error = std::move(why);
      runtime.Add(std::move(srv));
    };
    if (spawned >= kMaxMcpServers) {
      McpNote(name, "skipped (server limit reached)");
      skip("skipped: server limit reached");
      continue;
    }
    std::string config_error;
    if (!McpValidateServerConfig(name, conf, config_error)) {
      McpError(name, "invalid config: " + config_error);
      skip("invalid config: " + config_error);
      continue;
    }
    if (JsonValue(conf, "disabled", false)) {
      McpNote(name, "disabled");
      skip({});
      continue;
    }
    std::string type = JsonValue(conf, "type", "stdio");
    if (type != "stdio") {
      McpNote(name, "skipped (transport `" + type + "` not supported)");
      skip("transport `" + type + "` is not supported");
      continue;
    }
    bool required = JsonValue(conf, "required", true);
    int64_t id = -1;
    std::string start_error;
    if (!McpStartConfigured(*srv, config, id, start_error)) {
      McpError(name, start_error);
      srv->error = start_error;
      runtime.Add(std::move(srv));
      ++spawned;
      if (required) {
        return "required MCP server `" + name + "`: " + start_error;
      }
      continue;
    }
    // Queue the handshake now (the pipe buffers it); reap replies below.
    boots.push_back({srv.get(), required});
    srv->discovery_id = id;
    if (!required) srv->startup = McpStartupState::kDiscovering;
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
    if (!McpValidateDiscovery(McpAwait(s, s.discovery_id, remaining, false),
                              initialize_error)) {
      McpError(s.name, initialize_error);
      s.Shutdown();
      return "required MCP server `" + s.name + "`: " + initialize_error;
    }
    s.discovery_id = -1;

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

namespace {

std::string ConfigDir(const McpServer& server) {
  return JsonValue(server.config, "__uagent_config_dir", "");
}

// Drops the server's tools and starts a fresh process under the same config.
// Discovery gets the optional servers' startup window, then finishes at the
// next turn boundary.
void Restart(std::vector<Tool>& tools, McpRuntime& runtime,
             const McpServer& old, const RuntimeConfig& config) {
  const std::string provider = "mcp:" + old.name;
  if (std::erase_if(
          tools, [&](const Tool& tool) { return tool.provider == provider; })) {
    runtime.registry_changed = true;
  }
  auto fresh = std::make_unique<McpServer>();
  fresh->name = old.name;
  fresh->config = old.config;
  McpServer& server = runtime.Replace(old, std::move(fresh));
  if (JsonValue(server.config, "disabled", false)) return;
  // The same checks as at startup: a config that failed them still does.
  std::string invalid;
  if (!McpValidateServerConfig(server.name, server.config, invalid)) {
    server.error = "invalid config: " + invalid;
    return;
  }
  if (std::string type = JsonValue(server.config, "type", "stdio");
      type != "stdio") {
    server.error = "transport `" + type + "` is not supported";
    return;
  }
  int64_t id = -1;
  if (!McpStartConfigured(server, config, id, server.error)) return;
  server.discovery_id = id;
  server.startup = McpStartupState::kDiscovering;
  auto deadline = DeadlineAfter(config.mcp_startup_grace_s);
  while (server.alive && server.startup != McpStartupState::kReady &&
         std::chrono::steady_clock::now() < deadline) {
    if (McpAdvanceStartup(tools, server, config)) {
      runtime.registry_changed = true;
    }
    (void)poll(nullptr, 0, std::min(10, PollTimeoutMs(deadline)));
  }
}

// Sets or clears `disabled` for one server in the file that defines it.
bool WriteDisabled(const McpServer& server, bool disabled, std::string& error) {
  const std::string dir = ConfigDir(server);
  const bool project = dir == CanonicalCwd() && dir != UserHome();
  const std::string path = dir + "/.mcp.json";
  std::error_code ec;
  if (dir.empty() || std::filesystem::is_symlink(path, ec)) {
    error = "cannot edit " + path;
    return false;
  }
  json trust, file;
  if (project) {
    if (!ProjectConfigTrusted() || !ProjectTrustSnapshot(trust, error)) {
      error =
          "this project's MCP config changed since it was trusted; "
          "start a new conversation to review it";
      return false;
    }
    file = trust["mcp"];
  } else {
    std::string bytes;
    if (!ReadRegularFile(path, kMcpConfigBytes, bytes, error)) {
      return false;
    }
    file = json::parse(bytes, nullptr, false);
  }
  const json* servers = JsonObject(file, "mcpServers");
  if (!servers || !JsonObject(*servers, server.name.c_str())) {
    error = server.name + " is no longer defined in " + path;
    return false;
  }
  json& entry = file["mcpServers"][server.name];
  if (disabled) {
    entry["disabled"] = true;
  } else {
    entry.erase("disabled");
  }
  if (!AtomicWriteFile(path, JsonDump(file, 2) + "\n", kPrivateFileMode,
                       /*preserve_mode=*/true, error)) {
    return false;
  }
  // The person made exactly this edit, so trust follows it; anything else
  // that changed meanwhile no longer matches and is asked about again.
  return !project || WriteTrustRecord(dir,
                                      {{"format", 3},
                                       {"mcp", std::move(file)},
                                       {"config", trust["config"]}},
                                      error);
}

// The end of the server's own stderr usually says why it stopped.
std::string LogTail(const std::string& name) {
  std::ifstream file(McpLogPath(name), std::ios::binary | std::ios::ate);
  if (!file) return {};
  file.seekg(std::max<std::streamoff>(0, file.tellg() - std::streamoff{1000}));
  return {std::istreambuf_iterator<char>(file), {}};
}

}  // namespace

json McpStatus(const McpRuntime& runtime, const std::vector<Tool>& tools) {
  json servers = json::array();
  for (const auto& owned : runtime.Servers()) {
    const McpServer& server = *owned;
    const bool disabled = JsonValue(server.config, "disabled", false);
    const std::string state =
        server.alive
            ? (server.startup == McpStartupState::kReady ? "ready" : "starting")
        : disabled ? "disabled"
                   : "failed";
    const std::string provider = "mcp:" + server.name;
    std::string command = JsonValue(server.config, "command", "");
    for (const json& arg : JsonValue(server.config, "args", json::array())) {
      if (arg.is_string()) command += " " + arg.get<std::string>();
    }
    const std::string dir = ConfigDir(server);
    json row = {
        {"name", server.name},
        {"scope", dir == UserHome() ? "global" : "project"},
        {"file", dir + "/.mcp.json"},
        {"state", state},
        {"command", command},
        {"overrides", JsonValue(server.config, "__uagent_overrides", false)},
        {"tools",
         std::count_if(tools.begin(), tools.end(), [&](const Tool& tool) {
           return tool.provider == provider;
         })}};
    if (state == "failed") {
      // A server that exited on its own usually said why on its last line.
      std::string log = LogTail(server.name), reason = server.error;
      if (reason.empty()) {
        std::string_view text = log;
        while (!text.empty() && std::isspace(Byte(text.back()))) {
          text.remove_suffix(1);
        }
        reason = text.substr(text.find_last_of('\n') + 1);
      }
      row["error"] = reason.empty() ? "stopped" : reason;
      row["log"] = std::move(log);
    }
    servers.push_back(std::move(row));
  }
  return servers;
}

json McpControl(const json& request, std::vector<Tool>& tools,
                McpRuntime& runtime, const RuntimeConfig& config) {
  const std::string operation = JsonValue(request, "operation", "");
  const std::string name = JsonValue(request, "name", "");
  McpServer* found = nullptr;
  for (const auto& server : runtime.Servers()) {
    if (server->name == name) found = server.get();
  }
  if (!found) return {{"error", "no MCP server named " + name}};
  if (operation == "mcp_enable") {
    const bool enabled = JsonValue(request, "enabled", true);
    std::string error;
    if (!WriteDisabled(*found, !enabled, error)) return {{"error", error}};
    found->config["disabled"] = !enabled;
  } else if (operation != "mcp_restart") {
    return {{"error", "unknown MCP operation"}};
  }
  Restart(tools, runtime, *found, config);
  return {{"mcp", McpStatus(runtime, tools)}};
}

}  // namespace uagent
