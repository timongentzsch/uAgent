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
  static ConfigManager Capture(bool trust_project, double cli_budget,
                               bool cli_no_memory);

  RuntimeConfig Initialize();
  std::optional<ConfigReload> Reload(const RuntimeConfig& active);
  json DiagnosticJson(const RuntimeConfig& active) const;

 private:
  ConfigManager(RuntimeConfig::Values process, bool trust_project,
                double cli_budget, bool cli_no_memory);
  EffectiveConfigSnapshot Read() const;
  bool FilesChanged() const;

  RuntimeConfig::Values process_;
  bool trust_project_ = false;
  double cli_budget_ = -1;
  bool cli_no_memory_ = false;
  std::string custom_path_;
  std::string global_path_;
  std::string project_path_;
  EffectiveConfigSnapshot current_;
  std::vector<std::string> deferred_;
  bool initialized_ = false;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_EFFECTIVE_CONFIG_H_
