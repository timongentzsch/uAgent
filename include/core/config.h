// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_CONFIG_H_
#define UAGENT_INCLUDE_CORE_CONFIG_H_
// Private configuration and workspace trust. Shell exports beat a trusted
// ./.uagent/.config, which beats ~/.uagent/.config; project .env files are
// application data and are never imported.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/strings.h"

namespace uagent {

using EnvValues = std::map<std::string, std::string>;

// Parse without mutating the process. Only agent-owned keys are exported later;
// other entries remain available as interpolation sources without leaking every
// config value into child processes.
inline EnvValues ReadEnvValues(const std::string& path) {
  EnvValues values;
  std::ifstream f(path);
  if (!f) return values;
  std::string line;
  while (std::getline(f, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == '#') continue;
    if (line.starts_with("export ")) line = Trim(line.substr(7));
    size_t eq = line.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    std::string key = Trim(line.substr(0, eq));
    std::string val = Trim(line.substr(eq + 1));
    if (key.empty()) continue;
    values[key] = Unquote(val);
  }
  return values;
}

inline std::string ResolveEnvValue(const std::string& key,
                                   const EnvValues& values,
                                   std::set<std::string>& resolving,
                                   bool process_fallback = true) {
  if (process_fallback) {
    const char* inherited = getenv(key.c_str());
    if (inherited) return inherited;
  }
  auto found = values.find(key);
  if (found == values.end() || !resolving.insert(key).second) return "";
  const std::string& value = found->second;
  std::string out;
  for (size_t i = 0; i < value.size();) {
    if (value[i] != '$') {
      out += value[i++];
      continue;
    }
    if (i + 1 < value.size() && value[i + 1] == '$') {
      out += '$';
      i += 2;
      continue;
    }
    size_t begin = i + 1, end = begin;
    bool braced = begin < value.size() && value[begin] == '{';
    if (braced) {
      begin++;
      end = value.find('}', begin);
      if (end == std::string::npos) {
        out += value[i++];
        continue;
      }
    } else {
      while (end < value.size() &&
             (isalnum(static_cast<unsigned char>(value[end])) ||
              value[end] == '_')) {
        ++end;
      }
      if (end == begin) {
        out += value[i++];
        continue;
      }
    }
    std::string ref = value.substr(begin, end - begin);
    out += ResolveEnvValue(ref, values, resolving, process_fallback);
    i = braced ? end + 1 : end;
  }
  resolving.erase(key);
  return out;
}

inline std::string ExpandProcessEnv(const std::string& value) {
  EnvValues none;
  none["__uagent_value"] = value;
  std::set<std::string> resolving;
  return ResolveEnvValue("__uagent_value", none, resolving);
}

inline bool AgentConfigKey(const std::string& key) {
  return key.starts_with("UAGENT_") || key == "OPENROUTER_API_KEY" ||
         key == "OPENROUTER_MODEL" || key == "OPENROUTER_EFFORT";
}

inline bool ProjectMcpPresent() {
  std::error_code ec;
  return std::filesystem::is_regular_file(".mcp.json", ec);
}

inline bool ProjectAgentConfigPresent() {
  std::string path = ProjectConfigFilePath();
  std::error_code ec;
  return !path.empty() && std::filesystem::is_regular_file(path, ec);
}

// Either surface a workspace can use to reconfigure the agent.
inline bool ProjectConfigPresent() {
  return ProjectMcpPresent() || ProjectAgentConfigPresent();
}

inline bool ProjectMcpSnapshot(json& snapshot, std::string& error) {
  std::error_code ec;
  uintmax_t bytes = std::filesystem::file_size(".mcp.json", ec);
  int64_t max_bytes = McpConfigBytes();
  if (ec || bytes > static_cast<uintmax_t>(max_bytes)) {
    error = ec ? ec.message() : "configuration exceeds byte limit";
    return false;
  }
  std::ifstream file(".mcp.json");
  snapshot = json::parse(file, nullptr, false);
  if (!snapshot.is_object()) {
    error = "project .mcp.json is not a valid JSON object";
    return false;
  }
  return true;
}

// Trust covers exactly what the workspace can change: the servers .mcp.json
// spawns and the settings ./.uagent/.config exports. Both are stored parsed, so
// a reformat keeps trust while any value change revokes it. ReadEnvValues
// returns an ordered map, so the recorded object is stable.
inline bool ProjectTrustSnapshot(json& snapshot, std::string& error) {
  json mcp = nullptr;
  if (ProjectMcpPresent() && !ProjectMcpSnapshot(mcp, error)) return false;
  json config = nullptr;
  if (ProjectAgentConfigPresent()) {
    config = json::object();
    for (const auto& [key, value] : ReadEnvValues(ProjectConfigFilePath())) {
      config[key] = value;
    }
  }
  snapshot = {{"mcp", std::move(mcp)}, {"config", std::move(config)}};
  return true;
}

inline std::string TrustStorePath() {
  return UagentDir(kConfigDir) + "/trusted-projects.json";
}

inline json ReadTrustStore() {
  std::ifstream file(TrustStorePath());
  if (!file) return json::object();
  json value = json::parse(file, nullptr, false);
  return value.is_object() ? value : json::object();
}

// Records written before both surfaces were covered carry an older format and
// simply stop matching, so those workspaces are asked once more.
inline bool TrustRecordMatches(const json& record, const json& snapshot) {
  return record.is_object() && JsonValue(record, "format", 0) == 3 &&
         record.contains("mcp") && record["mcp"] == snapshot["mcp"] &&
         record.contains("config") && record["config"] == snapshot["config"];
}

// The out-parameter carries the approved .mcp.json alone: MCP registration
// consumes it directly, so a file swap after approval cannot change which
// commands are spawned.
inline bool ProjectConfigTrusted(json* trusted_mcp = nullptr) {
  json store = ReadTrustStore();
  std::string root = CanonicalCwd();
  json snapshot;
  std::string error;
  if (!ProjectTrustSnapshot(snapshot, error) || !store.contains(root) ||
      !TrustRecordMatches(store[root], snapshot)) {
    return false;
  }
  if (trusted_mcp) *trusted_mcp = std::move(snapshot["mcp"]);
  return true;
}

inline bool TrustProjectConfig(std::string& error,
                               json* trusted_mcp = nullptr) {
  json snapshot;
  if (!ProjectTrustSnapshot(snapshot, error)) return false;
  json store = ReadTrustStore();
  store[CanonicalCwd()] = {
      {"format", 3}, {"mcp", snapshot["mcp"]}, {"config", snapshot["config"]}};
  std::string path = TrustStorePath();
  std::string data = JsonDump(store, 2) + "\n";
  if (!AtomicWriteFile(path, data, kPrivateFileMode, /*preserve_mode=*/false,
                       error)) {
    return false;
  }
  if (trusted_mcp) *trusted_mcp = std::move(snapshot["mcp"]);
  return true;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_CONFIG_H_
