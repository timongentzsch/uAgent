// Copyright 2026 Timon Gentzsch

#include "include/core/effective_config.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "include/core/config.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/strings.h"

extern char** environ;

namespace uagent {
namespace {

FileStamp MergeFile(const std::string& path, const char* source,
                    const RuntimeConfig::Values& process,
                    RuntimeConfig::Values& effective, json& origins) {
  if (path.empty()) return {};
  EnvValues parsed;
  FileStamp after;
  bool stable = false;
  for (int attempt = 0; attempt < 3; ++attempt) {
    FileStamp before = SnapshotFile(path);
    parsed = ReadEnvValues(path);
    after = SnapshotFile(path);
    if (before == after) {
      stable = true;
      break;
    }
  }
  if (!stable) after.size = -2;  // force another boundary check
  if (!parsed.empty()) chmod(path.c_str(), kPrivateFileMode);
  RuntimeConfig::Values scope(parsed.begin(), parsed.end());
  for (const auto& [key, value] : process) scope[key] = value;
  for (const auto& [key, ignored] : parsed) {
    (void)ignored;
    if (!AgentConfigKey(key)) continue;
    std::set<std::string> resolving;
    effective[key] =
        ResolveEnvValue(key, scope, resolving, /*process_fallback=*/false);
    origins[key] = source;
  }
  return after;
}

std::vector<std::string> DifferentKeys(const RuntimeConfig& configured,
                                       const RuntimeConfig& active) {
  json wanted = configured.DiagnosticJson();
  json actual = active.DiagnosticJson();
  std::vector<std::string> keys;
  for (const auto& [key, value] : wanted.items()) {
    auto found = actual.find(key);
    if (found == actual.end() || *found != value) keys.push_back(key);
  }
  if (configured.web_search_api_key != active.web_search_api_key &&
      std::find(keys.begin(), keys.end(), "web_search_api_key") == keys.end()) {
    keys.push_back("web_search_api_key");
  }
  return keys;
}

}  // namespace

ConfigManager ConfigManager::Capture(bool trust_project,
                                     RuntimeConfig::Values cli) {
  RuntimeConfig::Values process;
  for (char** entry = environ; entry && *entry; ++entry) {
    std::string value(*entry);
    size_t equal = value.find('=');
    if (equal == std::string::npos || equal == 0) continue;
    process[value.substr(0, equal)] = value.substr(equal + 1);
  }
  return ConfigManager(std::move(process), trust_project, std::move(cli));
}

ConfigManager::ConfigManager(RuntimeConfig::Values process, bool trust_project,
                             RuntimeConfig::Values cli)
    : process_(std::move(process)),
      trust_project_(trust_project),
      cli_(std::move(cli)) {
  auto custom = process_.find("UAGENT_CONFIG_FILE");
  if (custom != process_.end()) custom_path_ = custom->second;
  global_path_ = UagentConfigPath();
  project_path_ = ProjectConfigFilePath();
}

EffectiveConfigSnapshot ConfigManager::Read() const {
  EffectiveConfigSnapshot snapshot;
  RuntimeConfig::Values effective;
  json origins = json::object();
  if (!custom_path_.empty()) {
    FileStamp stamp =
        MergeFile(custom_path_, "custom-config", process_, effective, origins);
    snapshot.files.push_back({custom_path_, stamp});
  } else {
    FileStamp global =
        MergeFile(global_path_, "global-config", process_, effective, origins);
    snapshot.files.push_back({global_path_, global});
    if (trust_project_) {
      FileStamp project = MergeFile(project_path_, "project-config", process_,
                                    effective, origins);
      snapshot.files.push_back({project_path_, project});
    }
  }
  for (const auto& [key, value] : process_) {
    if (!AgentConfigKey(key)) continue;
    effective[key] = value;
    origins[key] = "environment";
  }

  for (const auto& [key, value] : cli_) {
    effective[key] = value;
    origins[key] = "cli";
  }
  snapshot.config = RuntimeConfig::FromValues(effective);
  snapshot.values = effective;
  snapshot.sources = std::move(origins);
  snapshot.fingerprint = JsonDump(effective);
  return snapshot;
}

RuntimeConfig ConfigManager::Initialize() {
  current_ = Read();
  for (const auto& [key, value] : current_.values) {
    setenv(key.c_str(), value.c_str(), cli_.contains(key) ? 1 : 0);
  }
  initialized_ = true;
  return current_.config;
}

bool ConfigManager::FilesChanged() const {
  if (!initialized_) return true;
  for (const auto& [path, stamp] : current_.files) {
    if (SnapshotFile(path) != stamp) return true;
  }
  return false;
}

std::optional<ConfigReload> ConfigManager::Reload(const RuntimeConfig& active) {
  if (!FilesChanged()) return std::nullopt;
  EffectiveConfigSnapshot next = Read();
  if (next.fingerprint == current_.fingerprint) {
    current_ = std::move(next);
    return std::nullopt;
  }
  ConfigReload reload;
  reload.active = active;
  reload.applied = reload.active.ApplyTurnReload(next.config);
  reload.deferred = DifferentKeys(next.config, reload.active);
  std::set<std::string> deferred(reload.deferred.begin(),
                                 reload.deferred.end());
  for (const auto& [key, value] : next.values) {
    auto old_value = current_.values.find(key);
    bool changed =
        old_value == current_.values.end() || old_value->second != value;
    if (changed && RuntimeConfigField(key).empty()) deferred.insert(key);
  }
  for (const auto& entry : current_.values) {  // keys the reload dropped
    if (!next.values.contains(entry.first) &&
        RuntimeConfigField(entry.first).empty()) {
      deferred.insert(entry.first);
    }
  }
  reload.deferred.assign(deferred.begin(), deferred.end());
  deferred_ = reload.deferred;
  current_ = std::move(next);
  return reload;
}

json ConfigManager::DiagnosticJson(const RuntimeConfig& active) const {
  return {{"active", active.DiagnosticJson()},
          {"configured", current_.config.DiagnosticJson()},
          {"sources", current_.sources},
          {"provenance", current_.config.ProvenanceJson(current_.sources)},
          {"restart_required", deferred_}};
}

}  // namespace uagent
