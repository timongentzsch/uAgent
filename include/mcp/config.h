// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MCP_CONFIG_H_
#define UAGENT_INCLUDE_MCP_CONFIG_H_
// Server configuration: validation of anything a config file asks to
// spawn, then the merge of user and trusted project sources.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/term.h"
#include "include/mcp/server.h"

namespace uagent {

inline bool McpValidateServerConfig(const std::string& name, const json& conf,
                                    std::string& error) {
  if (!conf.is_object()) {
    error = "server entry must be an object";
    return false;
  }
  auto require_type = [&](const char* field, json::value_t type,
                          const char* expected) {
    if (!conf.contains(field) || conf[field].type() == type) return true;
    error = std::string("`") + field + "` must be " + expected;
    return false;
  };
  if (!require_type("command", json::value_t::string, "a string") ||
      !require_type("type", json::value_t::string, "a string") ||
      !require_type("cwd", json::value_t::string, "a string") ||
      !require_type("args", json::value_t::array, "an array") ||
      !require_type("env", json::value_t::object, "an object") ||
      !require_type("tools", json::value_t::array, "an array") ||
      !require_type("roots", json::value_t::array, "an array") ||
      !require_type("trust", json::value_t::boolean, "a boolean") ||
      !require_type("disabled", json::value_t::boolean, "a boolean")) {
    return false;
  }
  if (JsonValue(conf, "command", "").empty()) {
    error = "`command` is required and must not be empty";
    return false;
  }
  if (conf.contains("args")) {
    for (const json& value : conf["args"]) {
      if (!value.is_string()) {
        error = "every `args` entry must be a string";
        return false;
      }
    }
  }
  if (conf.contains("env")) {
    for (const auto& [key, value] : conf["env"].items()) {
      if (key.empty() || !value.is_string()) {
        error = "every `env` entry must have a nonempty key and string value";
        return false;
      }
    }
  }
  if (conf.contains("tools")) {
    for (const json& value : conf["tools"]) {
      if (!value.is_string()) {
        error = "every `tools` allowlist entry must be a string";
        return false;
      }
    }
  }
  if (conf.contains("roots")) {
    for (const json& value : conf["roots"]) {
      if (!value.is_string() || value.get<std::string>().empty()) {
        error = "every `roots` entry must be a nonempty string";
        return false;
      }
    }
  }
  static const std::set<std::string> kNown = {
      "type",  "command", "args",  "env",      "cwd",
      "tools", "roots",   "trust", "disabled", "__uagent_config_dir"};
  for (const auto& [field, ignored] : conf.items()) {
    (void)ignored;
    if (!kNown.contains(field)) {
      McpNote(name, "unknown config field `" + field + "` ignored");
    }
  }
  return true;
}

inline std::string McpFileUri(const std::filesystem::path& path) {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string uri = "file://";
  for (unsigned char byte : path.string()) {
    bool safe = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                byte == '.' || byte == '~' || byte == '/';
    if (safe) {
      uri += static_cast<char>(byte);
    } else {
      uri += '%';
      uri += kHex[byte >> 4];
      uri += kHex[byte & 0x0f];
    }
  }
  return uri;
}

inline bool McpResolveRoots(const json& conf, const std::string& configured,
                            json& roots, std::string& error) {
  std::vector<std::string> requested;
  std::filesystem::path base = CanonicalCwd();
  if (conf.contains("roots")) {
    for (const json& value : conf["roots"]) {
      requested.push_back(value.get<std::string>());
    }
    std::string config_dir = JsonValue(conf, "__uagent_config_dir", "");
    if (!config_dir.empty()) base = config_dir;
  } else if (!configured.empty()) {
    requested = SplitPathList(configured);
  } else {
    requested.push_back(base.string());
  }

  roots = json::array();
  std::set<std::string> seen;
  for (const std::string& value : requested) {
    if (value.empty()) continue;
    std::filesystem::path path = ExpandProcessEnv(value);
    if (path.empty()) {
      error = "invalid MCP root `" + value + "` (expanded to empty)";
      return false;
    }
    if (path.is_relative()) path = base / path;
    std::error_code ec;
    path = std::filesystem::weakly_canonical(path, ec);
    if (ec || !std::filesystem::exists(path, ec)) {
      error = "invalid MCP root `" + value + "`";
      return false;
    }
    std::string canonical = path.string();
    if (!seen.insert(canonical).second) continue;
    std::string name = path.filename().string();
    roots.push_back(
        {{"uri", McpFileUri(path)}, {"name", name.empty() ? canonical : name}});
  }
  if (roots.empty()) {
    error = "MCP roots resolved to an empty set";
    return false;
  }
  return true;
}

// A trusted ./.mcp.json then ~/.mcp.json — the project layer wins.
// The credential/config loader never imports a project .env.
inline json McpLoadConfig(const json& trusted_project, size_t max_bytes) {
  auto read = [max_bytes](const std::string& path) -> json {
    std::error_code ec;
    uintmax_t bytes = std::filesystem::file_size(path, ec);
    if (!ec && max_bytes > 0 && bytes > max_bytes) {
      McpError(path, "configuration exceeds byte limit");
      return json::object();
    }
    std::ifstream f(path);
    if (!f) return json::object();
    json j = json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      printf("%smcp: %s is not valid JSON — ignored%s\n", RED(), path.c_str(),
             RST());
      return json::object();
    }
    return j.contains("mcpServers") && j["mcpServers"].is_object()
               ? j["mcpServers"]
               : json::object();
  };
  auto annotate = [](json servers, const std::string& config_dir) {
    if (!servers.is_object()) return json::object();
    for (auto& [name, conf] : servers.items()) {
      (void)name;
      if (conf.is_object()) conf["__uagent_config_dir"] = config_dir;
    }
    return servers;
  };
  json cfg =
      trusted_project.is_object() && trusted_project.contains("mcpServers")
          ? annotate(trusted_project["mcpServers"], CanonicalCwd())
          : json::object();
  std::string home_dir = UserHome();
  json home = home_dir.empty()
                  ? json::object()
                  : annotate(read(home_dir + "/.mcp.json"), home_dir);
  for (auto& [name, conf] : home.items()) {
    if (!cfg.contains(name)) cfg[name] = conf;
  }
  return cfg;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_CONFIG_H_
