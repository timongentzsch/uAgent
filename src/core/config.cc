// Copyright 2026 Timon Gentzsch

#include "include/core/config.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include "include/core/config_document.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/private_store.h"
#include "include/core/strings.h"

namespace uagent {

EnvValues ParseEnvValues(std::istream& input) {
  EnvValues values;
  std::string line;
  while (std::getline(input, line)) {
    ConfigAssignment assignment;
    if (!ParseConfigAssignment(line, assignment)) continue;
    values[assignment.key] = Unquote(assignment.value);
  }
  return values;
}

EnvValues ParseEnvValues(const std::string& text) {
  std::istringstream input(text);
  return ParseEnvValues(input);
}

EnvValues ReadEnvValues(const std::string& path) {
  std::ifstream f(path);
  if (!f) return {};
  return ParseEnvValues(f);
}

std::string ResolveEnvValue(const std::string& key, const EnvValues& values,
                            std::set<std::string>& resolving,
                            bool process_fallback) {
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

std::string ExpandProcessEnv(const std::string& value) {
  EnvValues none;
  none["__uagent_value"] = value;
  std::set<std::string> resolving;
  return ResolveEnvValue("__uagent_value", none, resolving);
}

bool AgentConfigKey(const std::string& key) {
  // A parent hands UAGENT_INTERNAL_* names to its children through the
  // environment; they are not settings, so no config file may carry one.
  if (key.starts_with("UAGENT_INTERNAL_")) return false;
  return key.starts_with("UAGENT_") || key == "OPENROUTER_API_KEY" ||
         key == "OPENROUTER_MODEL" || key == "OPENROUTER_EFFORT";
}

bool ProjectMcpPresent() {
  std::error_code ec;
  return std::filesystem::is_regular_file(".mcp.json", ec);
}

bool ProjectAgentConfigPresent() {
  std::string path = ProjectConfigFilePath();
  std::error_code ec;
  return !path.empty() && std::filesystem::is_regular_file(path, ec);
}

bool ProjectMcpSnapshot(json& snapshot, std::string& error) {
  std::error_code ec;
  uintmax_t bytes = std::filesystem::file_size(".mcp.json", ec);
  if (ec || bytes > kMcpConfigBytes) {
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

bool ProjectTrustSnapshot(json& snapshot, std::string& error) {
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

std::string TrustStorePath() {
  return UagentDir(kConfigDir) + "/" + kTrustStoreFile;
}

bool WriteTrustRecord(const std::string& root, json record,
                      std::string& error) {
  PrivateJsonStore store(kTrustStoreFile, json::object(), kTrustStoreBytes,
                         error);
  if (!store.Ready()) return false;
  if (!store.Data().is_object()) store.Data() = json::object();
  store.Data()[root] = std::move(record);
  return store.Save(error);
}

json ReadTrustStore() {
  std::ifstream file(TrustStorePath());
  if (!file) return json::object();
  json value = json::parse(file, nullptr, false);
  return value.is_object() ? value : json::object();
}

bool TrustRecordMatches(const json& record, const json& snapshot) {
  return record.is_object() && JsonValue(record, "format", 0) == 3 &&
         record.contains("mcp") && record["mcp"] == snapshot["mcp"] &&
         record.contains("config") && record["config"] == snapshot["config"];
}

bool ProjectConfigTrusted(json* trusted_mcp) {
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

bool RestampProjectConfigTrust(std::string& error) {
  json store = ReadTrustStore();
  std::string root = CanonicalCwd();
  if (!store.contains(root)) {
    error = "this workspace has no trust record to update";
    return false;
  }
  json record = store[root];
  json snapshot;
  if (!ProjectTrustSnapshot(snapshot, error)) return false;
  if (JsonValue(record, "format", 0) != 3 || !record.contains("mcp") ||
      record["mcp"] != snapshot["mcp"]) {
    error = "project .mcp.json changed, so trust must be granted again";
    return false;
  }
  return WriteTrustRecord(
      root,
      {{"format", 3}, {"mcp", record["mcp"]}, {"config", snapshot["config"]}},
      error);
}

bool TrustProjectConfig(std::string& error, json* trusted_mcp) {
  json snapshot;
  if (!ProjectTrustSnapshot(snapshot, error)) return false;
  if (!WriteTrustRecord(CanonicalCwd(),
                        {{"format", 3},
                         {"mcp", snapshot["mcp"]},
                         {"config", snapshot["config"]}},
                        error)) {
    return false;
  }
  if (trusted_mcp) *trusted_mcp = std::move(snapshot["mcp"]);
  return true;
}

}  // namespace uagent
