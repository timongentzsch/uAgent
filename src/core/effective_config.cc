// Copyright 2026 Timon Gentzsch

#include "include/core/effective_config.h"

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
#include "include/core/settings_store.h"
#include "include/core/strings.h"

extern char** environ;

namespace uagent {
namespace {

// One saved scope laid over what is below it. A value may refer to another
// name in its own scope or in the environment as $NAME; only names the
// registry knows become settings.
void MergeScope(const SettingValues& saved, const char* source,
                const RuntimeConfig::Values& process,
                EffectiveConfigSnapshot& snapshot) {
  RuntimeConfig::Values scope(saved.begin(), saved.end());
  for (const auto& [key, value] : process) scope[key] = value;
  for (const auto& [key, value] : saved) {
    if (!AgentConfigKey(key)) continue;
    std::set<std::string> resolving;
    snapshot.values[key] =
        ResolveEnvValue(key, scope, resolving, /*process_fallback=*/false);
    snapshot.sources[key] = source;
    // As saved: a reference stays a reference where the scope is shown.
    snapshot.layers[source][key] = value;
  }
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
                                     RuntimeConfig::Values cli,
                                     std::optional<std::string> folder) {
  RuntimeConfig::Values process;
  for (char** entry = environ; entry && *entry; ++entry) {
    std::string value(*entry);
    size_t equal = value.find('=');
    if (equal == std::string::npos || equal == 0) continue;
    process[value.substr(0, equal)] = value.substr(equal + 1);
  }
  return ConfigManager(std::move(process), trust_project, std::move(cli),
                       folder ? std::move(*folder) : CanonicalCwd());
}

ConfigManager::ConfigManager(RuntimeConfig::Values process, bool trust_project,
                             RuntimeConfig::Values cli, std::string folder)
    : process_(std::move(process)),
      trust_project_(trust_project),
      cli_(std::move(cli)),
      folder_(std::move(folder)) {}

EffectiveConfigSnapshot ConfigManager::Read() const {
  EffectiveConfigSnapshot snapshot;
  const SavedSettings saved = ReadSettings(folder_, trust_project_);
  snapshot.stamp = saved.stamp;
  snapshot.error = saved.error;
  snapshot.warning = saved.warning;
  MergeScope(saved.all, "user", process_, snapshot);
  MergeScope(saved.project, "project", process_, snapshot);
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
    const auto held = layers.find(std::string(scope.source));
    if (held == layers.end()) continue;
    if (auto found = held->second.find(key); found != held->second.end()) {
      value = found->second;
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
  PublishSettings(current_.values);
  initialized_ = true;
  return current_.config;
}

std::optional<ConfigReload> ConfigManager::Reload(const RuntimeConfig& active) {
  // A read that failed is tried again; one that still fails changes nothing:
  // empty layers would otherwise pass for everything having been reset.
  if (initialized_ && current_.error.empty() &&
      SnapshotFile(SettingsPath()) == current_.stamp) {
    return std::nullopt;
  }
  EffectiveConfigSnapshot next = Read();
  if (initialized_ && !next.error.empty()) {
    current_.error = next.error;
    return std::nullopt;
  }
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
