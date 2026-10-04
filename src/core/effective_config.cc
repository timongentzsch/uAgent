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
#include "include/core/config_registry.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/runtime_config.h"
#include "include/core/strings.h"

extern char** environ;

namespace uagent {
namespace {

FileStamp MergeFile(const std::string& path, const char* source,
                    const RuntimeConfig::Values& process,
                    EffectiveConfigSnapshot& snapshot) {
  RuntimeConfig::Values& effective = snapshot.values;
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
    snapshot.sources[key] = source;
    // As written: a reference stays a reference where the file is shown.
    snapshot.layers[source][key] = parsed[key];
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
  if (!custom_path_.empty()) {
    FileStamp stamp = MergeFile(custom_path_, "file", process_, snapshot);
    snapshot.files.emplace_back(custom_path_, stamp);
  } else {
    FileStamp global = MergeFile(global_path_, "user", process_, snapshot);
    snapshot.files.emplace_back(global_path_, global);
    if (trust_project_) {
      FileStamp project =
          MergeFile(project_path_, "project", process_, snapshot);
      snapshot.files.emplace_back(project_path_, project);
    }
  }
  auto layer = [&](const char* source, const RuntimeConfig::Values& held,
                   bool filter) {
    for (const auto& [key, value] : held) {
      if (filter && !AgentConfigKey(key)) continue;
      snapshot.values[key] = value;
      snapshot.sources[key] = source;
      snapshot.layers[source][key] = value;
    }
  };
  layer("environment", process_, true);
  layer("cli", cli_, false);
  layer("conversation", Conversation(), false);
  snapshot.config = RuntimeConfig::FromValues(snapshot.values);
  return snapshot;
}

std::string EffectiveConfigSnapshot::Inherited(const std::string& key) const {
  std::string value;
  for (const ConfigScopeName& scope : kConfigScopes) {
    if (scope.persisted == kScopeConversation) continue;
    // A file named by UAGENT_CONFIG_FILE stands where the user's would.
    for (std::string_view source :
         {scope.source,
          std::string_view(scope.persisted == kScopeUser ? "file" : "")}) {
      const auto held = layers.find(std::string(source));
      if (held == layers.end()) continue;
      if (auto found = held->second.find(key); found != held->second.end()) {
        value = found->second;
      }
    }
  }
  return value;
}

void ConfigManager::ChooseForConversation(const std::string& key,
                                          const std::string& value) {
  std::lock_guard lock(*conversation_mutex_);
  if (value.empty()) {
    conversation_.erase(key);
  } else {
    conversation_[key] = value;
  }
}

RuntimeConfig::Values ConfigManager::Conversation() const {
  std::lock_guard lock(*conversation_mutex_);
  return conversation_;
}

RuntimeConfig ConfigManager::Initialize() {
  current_ = Read();
  for (const auto& [key, value] : current_.values) {
    setenv(key.c_str(), value.c_str(), cli_.contains(key) ? 1 : 0);
  }
  PublishSettings(current_.values);
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
  if (next.values == current_.values) {
    current_ = std::move(next);
    return std::nullopt;
  }
  ConfigReload reload;
  reload.active = active;
  reload.applied = reload.active.ApplyTurnReload(next.config);
  reload.deferred = DifferentKeys(next.config, reload.active);
  std::set<std::string> deferred(reload.deferred.begin(),
                                 reload.deferred.end());
  // Unbound settings are read on use from the published values.
  auto unbound = [&](const std::string& key) {
    if (!RuntimeConfigField(key).empty()) return;
    const ConfigDescriptor* descriptor = FindConfigDescriptor(key);
    if (descriptor && descriptor->reload == ReloadPolicy::kNextUserTurn) {
      reload.applied.push_back(key);
    } else {
      deferred.insert(key);
    }
  };
  for (const auto& [key, value] : next.values) {
    auto old_value = current_.values.find(key);
    if (old_value == current_.values.end() || old_value->second != value) {
      unbound(key);
    }
  }
  for (const auto& entry : current_.values) {  // keys the reload dropped
    if (!next.values.contains(entry.first)) unbound(entry.first);
  }
  reload.deferred.assign(deferred.begin(), deferred.end());
  deferred_ = reload.deferred;
  current_ = std::move(next);
  PublishSettings(current_.values);
  return reload;
}

json ConfigManager::DiagnosticJson(const RuntimeConfig& active) const {
  return {{"active", active.DiagnosticJson()},
          {"configured", current_.config.DiagnosticJson()},
          {"sources", current_.sources},
          {"restart_required", deferred_}};
}

}  // namespace uagent
