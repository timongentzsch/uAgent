// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_EFFECTIVE_CONFIG_H_
#define UAGENT_INCLUDE_CORE_EFFECTIVE_CONFIG_H_
// Immutable configuration snapshots assembled without mutating process state.
// Reload checks file stamps synchronously at a user-turn boundary; no watcher
// thread and no mid-turn configuration mutation exist.

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/file_watch.h"
#include "include/core/json.h"

namespace uagent {

struct EffectiveConfigSnapshot {
  RuntimeConfig config;
  RuntimeConfig::Values values;
  json sources = json::object();
  std::string fingerprint;
  std::vector<std::pair<std::string, FileStamp>> files;
};

struct ConfigReload {
  RuntimeConfig active;
  std::vector<std::string> applied;
  std::vector<std::string> deferred;
};

class ConfigManager {
 public:
  // `cli` holds UAGENT_* values named on the command line. They sit above the
  // environment layer and are re-applied with overwrite, so a flag beats an
  // inherited variable however the session was launched.
  static ConfigManager Capture(bool trust_project, RuntimeConfig::Values cli);

  RuntimeConfig Initialize();
  std::optional<ConfigReload> Reload(const RuntimeConfig& active);
  json DiagnosticJson(const RuntimeConfig& active) const;

 private:
  ConfigManager(RuntimeConfig::Values process, bool trust_project,
                RuntimeConfig::Values cli);
  EffectiveConfigSnapshot Read() const;
  bool FilesChanged() const;

  RuntimeConfig::Values process_;
  bool trust_project_ = false;
  RuntimeConfig::Values cli_;
  std::string custom_path_;
  std::string global_path_;
  std::string project_path_;
  EffectiveConfigSnapshot current_;
  std::vector<std::string> deferred_;
  bool initialized_ = false;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_EFFECTIVE_CONFIG_H_
