// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_EFFECTIVE_CONFIG_H_
#define UAGENT_INCLUDE_CORE_EFFECTIVE_CONFIG_H_
// Immutable configuration snapshots assembled without mutating process state.
// Reload checks file stamps synchronously at a user-turn boundary; no watcher
// thread and no mid-turn configuration mutation exist.

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/file_watch.h"
#include "include/core/json.h"
#include "include/core/runtime_config.h"

namespace uagent {

struct EffectiveConfigSnapshot {
  RuntimeConfig config;
  RuntimeConfig::Values values;
  json sources = json::object();
  // What each scope holds, by its source name, as that scope spells it: the
  // answer to "where is this set, and what does it override". `values` is
  // their merge, lowest first: user, project, environment, cli, conversation.
  std::map<std::string, RuntimeConfig::Values> layers;
  // The saved settings' stamp when they were read, and why they are not all
  // here if they are not.
  FileStamp stamp;
  std::string error;
  // What the saved settings hold that was not taken; the rest applies.
  std::string warning;

  // What `key` would be without the conversation's own choice: the value a
  // conversation's control offers as its default. Empty when no scope sets it.
  std::string Inherited(const std::string& key) const;
};

struct ConfigReload {
  RuntimeConfig active;
  std::vector<std::string> applied;
  std::vector<std::string> deferred;
};

class ConfigManager {
 public:
  // `cli` holds UAGENT_* values named on the command line. They sit above the
  // environment layer, so a flag beats an inherited variable however the
  // session was launched.
  // `folder` is the project whose saved settings apply: this process's own
  // unless one is named (the web host has none of its own, so it names the
  // one a request is about, or none).
  static ConfigManager Capture(RuntimeConfig::Values cli,
                               std::optional<std::string> folder = {});
  // The resolved values as they are now, publishing nothing.
  EffectiveConfigSnapshot Read() const;
  // The project whose saved settings apply.
  const std::string& Folder() const { return folder_; }
  // Why the saved settings were not all read or not all taken, as of the
  // last read.
  const std::string& Problem() const {
    return current_.error.empty() ? current_.warning : current_.error;
  }
  // What one conversation chose for itself, above every other scope. Only
  // settings whose descriptor allows the conversation scope belong here; an
  // empty value takes the choice back. Safe beside Read() on another thread.
  void ChooseForConversation(const std::string& key, const std::string& value);
  RuntimeConfig::Values Conversation() const;

  RuntimeConfig Initialize();
  std::optional<ConfigReload> Reload(const RuntimeConfig& active);
  json DiagnosticJson(const RuntimeConfig& active) const;

 private:
  ConfigManager(RuntimeConfig::Values process, RuntimeConfig::Values cli,
                std::string folder);

  RuntimeConfig::Values process_;
  RuntimeConfig::Values cli_;
  RuntimeConfig::Values conversation_;
  std::unique_ptr<std::mutex> conversation_mutex_ =
      std::make_unique<std::mutex>();
  std::string folder_;
  EffectiveConfigSnapshot current_;
  std::vector<std::string> deferred_;
  bool initialized_ = false;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_EFFECTIVE_CONFIG_H_
