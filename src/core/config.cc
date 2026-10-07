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

#include "include/core/config_registry.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/private_store.h"
#include "include/core/strings.h"

namespace uagent {

// KEY=value lines, as the text config files of earlier versions and .env
// files spell them: `export ` is allowed, # starts a comment line.
EnvValues ParseEnvValues(std::istream& input) {
  EnvValues values;
  std::string line;
  while (std::getline(input, line)) {
    std::string text = Trim(line);
    if (text.starts_with("export ")) text = Trim(text.substr(7));
    const size_t equals = text.find('=');
    if (text.empty() || text[0] == '#' || equals == std::string::npos) continue;
    const std::string key = Trim(text.substr(0, equals));
    if (!key.empty()) values[key] = Unquote(Trim(text.substr(equals + 1)));
  }
  return values;
}

EnvValues ParseEnvValues(const std::string& text) {
  std::istringstream input(text);
  return ParseEnvValues(input);
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
  // A saved setting may be referred to like a variable: it is no longer one.
  EnvValues settings = CurrentSettings();
  settings["__uagent_value"] = value;
  std::set<std::string> resolving;
  return ResolveEnvValue("__uagent_value", settings, resolving);
}

bool AgentConfigKey(const std::string& key) {
  // A parent hands UAGENT_INTERNAL_* names to its children through the
  // environment; they are not settings, so no config file may carry one.
  if (key.starts_with("UAGENT_INTERNAL_")) return false;
  return key.starts_with("UAGENT_") || key == "OPENROUTER_API_KEY";
}

bool ProjectMcpPresent() {
  std::error_code ec;
  return std::filesystem::is_regular_file(".mcp.json", ec);
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
  snapshot = {{"mcp", std::move(mcp)}};
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
  // What an earlier version approved as the project's config stays with the
  // record until the settings import has taken it over.
  if (const json* approved = JsonObject(store.Data(), root.c_str());
      approved && approved->contains("config") && !record.contains("config")) {
    record["config"] = (*approved)["config"];
  }
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
         record.contains("mcp") && record["mcp"] == snapshot["mcp"];
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

bool TrustProjectConfig(std::string& error, json* trusted_mcp) {
  json snapshot;
  if (!ProjectTrustSnapshot(snapshot, error)) return false;
  if (!WriteTrustRecord(CanonicalCwd(),
                        {{"format", 3}, {"mcp", snapshot["mcp"]}}, error)) {
    return false;
  }
  if (trusted_mcp) *trusted_mcp = std::move(snapshot["mcp"]);
  return true;
}

}  // namespace uagent
