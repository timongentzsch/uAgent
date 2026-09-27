// Copyright 2026 Timon Gentzsch
#include <string>
#include <utility>
#include <vector>

#include "include/app/config_proposal.h"
#include "include/app/self_description.h"
#include "include/core/config.h"
#include "include/core/config_registry.h"
#include "include/core/effective_config.h"
#include "include/core/env.h"
#include "include/core/limits.h"

namespace uagent {
json ConfigurationControl(const json& request, const ConfigManager& manager,
                          const RuntimeConfig& active, bool project_trusted) {
  std::string operation = JsonValue(request, "operation", "get");
  json effects = json::array();
  // Each file's own values, so a scope can tell what it sets from what it
  // inherits. The project layer counts only while it is trusted.
  auto layer = [&](ConfigProposalScope scope) {
    if (scope == ConfigProposalScope::kUser) {
      return ReadEnvValues(UagentConfigPath());
    }
    return project_trusted ? ReadEnvValues(ProjectConfigFilePath())
                           : EnvValues{};
  };
  if (operation == "apply" || operation == "reset") {
    ConfigProposalScope scope = ConfigProposalScope::kUser;
    if (!ParseConfigScope(JsonValue(request, "scope", "user"), scope)) {
      return {{"error", "invalid configuration scope"}};
    }
    std::vector<ConfigChange> changes;
    std::string error;
    if (operation == "apply") {
      if (!ParseConfigChanges(request, changes, error)) {
        return {{"error", error}};
      }
    } else {
      // Every setting the scope overrides, except secrets: a reset must not
      // leave the agent without its keys.
      for (const auto& [key, value] : layer(scope)) {
        const ConfigDescriptor* descriptor = FindConfigDescriptor(key);
        if (descriptor && descriptor->sensitivity == Sensitivity::kPublic &&
            !value.empty()) {
          changes.push_back({.key = key, .value = "", .unset = true});
        }
      }
    }
    // A reset of a scope that overrides nothing has nothing to write.
    if (!changes.empty()) {
      auto proposal = PrepareConfigProposal(scope, changes, manager,
                                            project_trusted, true);
      if (!proposal.ok) return {{"error", proposal.error}};
      if (!CommitConfigProposal(proposal, error)) return {{"error", error}};
      for (const auto& effect : proposal.effects) {
        effects.push_back({{"key", effect.key},
                           {"effect", ConfigEffectName(effect.effect)}});
      }
    }
  } else if (operation != "get") {
    return {{"error", "unknown configuration operation"}};
  }
  // A fresh read: an apply above has just changed the files.
  auto configured = manager.Read();
  const EnvValues user = layer(ConfigProposalScope::kUser);
  const EnvValues project = layer(ConfigProposalScope::kProject);
  json settings =
      ConfigSettingsJson(configured.sources, active.DiagnosticJson(),
                         JsonValue(request, "name", ""));
  for (json& setting : settings) {
    const std::string name = setting["name"];
    const auto* descriptor = FindConfigDescriptor(name);
    setting["scopes"] = json::array();
    if (descriptor->scopes & kScopeUser) setting["scopes"].push_back("user");
    if (descriptor->scopes & kScopeProject) {
      setting["scopes"].push_back("project");
    }
    auto value = configured.values.find(name);
    // What each file sets; a secret reports only that it is set.
    const bool secret = descriptor->sensitivity != Sensitivity::kPublic;
    for (const auto& [scope, values] : {std::pair{"user", &user},
                                        std::pair{"project", &project}}) {
      auto own = values->find(name);
      if (own == values->end() || own->second.empty()) continue;
      setting[scope] = secret ? json(true) : json(own->second);
    }
    setting["value"] = descriptor->sensitivity != Sensitivity::kPublic
                           ? json(nullptr)
                       : value == configured.values.end() ? setting["default"]
                                                          : json(value->second);
  }
  return {{"settings", settings},
          {"effects", effects},
          {"project_trusted", project_trusted}};
}
}  // namespace uagent
