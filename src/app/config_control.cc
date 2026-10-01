// Copyright 2026 Timon Gentzsch
#include <string>
#include <vector>

#include "include/app/config_proposal.h"
#include "include/app/self_description.h"
#include "include/core/config.h"
#include "include/core/config_registry.h"
#include "include/core/effective_config.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/limits.h"

namespace uagent {
json ConfigurationControl(const json& request, const ConfigManager& manager,
                          bool project_trusted) {
  std::string operation = JsonValue(request, "operation", "get");
  json effects = json::array();
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
      // Secrets stay: a reset must not leave the agent without its keys.
      const EnvValues own = scope == ConfigProposalScope::kUser
                                ? ReadEnvValues(UagentConfigPath())
                            : project_trusted
                                ? ReadEnvValues(ProjectConfigFilePath())
                                : EnvValues{};
      for (const auto& [key, value] : own) {
        const ConfigDescriptor* descriptor = FindConfigDescriptor(key);
        if (descriptor && descriptor->sensitivity == Sensitivity::kPublic &&
            !value.empty()) {
          changes.push_back({.key = key, .value = "", .unset = true});
        }
      }
    }
    if (!changes.empty()) {
      auto proposal =
          PrepareConfigProposal(scope, changes, manager, project_trusted, true);
      if (!proposal.ok) return {{"error", proposal.error}};
      if (!CommitConfigProposal(proposal, error)) return {{"error", error}};
      for (const auto& effect : proposal.effects) {
        effects.push_back({{"key", effect.key},
                           {"effect", ConfigEffectToken(effect.effect)},
                           {"text", ConfigEffectName(effect.effect)}});
      }
    }
  } else if (operation != "get") {
    return {{"error", "unknown configuration operation"}};
  }
  return {{"settings",
           ConfigSettingsJson(manager.Read(), ReadEnvValues(UagentConfigPath()),
                              JsonValue(request, "name", ""))},
          {"effects", effects}};
}
}  // namespace uagent
