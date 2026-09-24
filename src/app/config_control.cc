// Copyright 2026 Timon Gentzsch
#include <string>
#include <utility>
#include <vector>

#include "include/app/config_proposal.h"
#include "include/app/self_description.h"
#include "include/core/config_registry.h"
#include "include/core/effective_config.h"
#include "include/core/env.h"
#include "include/core/limits.h"

namespace uagent {
json ConfigurationControl(const json& request, const ConfigManager& manager,
                          const RuntimeConfig& active, bool project_trusted) {
  std::string operation = JsonValue(request, "operation", "get");
  json effects = json::array();
  if (operation == "apply") {
    ConfigProposalScope scope = ConfigProposalScope::kUser;
    if (!ParseConfigScope(JsonValue(request, "scope", "user"), scope)) {
      return {{"error", "invalid configuration scope"}};
    }
    std::vector<ConfigChange> changes;
    std::string error;
    if (!ParseConfigChanges(request, changes, error)) return {{"error", error}};
    auto proposal =
        PrepareConfigProposal(scope, changes, manager, project_trusted, true);
    if (!proposal.ok) return {{"error", proposal.error}};
    if (!CommitConfigProposal(proposal, error)) return {{"error", error}};
    for (const auto& effect : proposal.effects) {
      effects.push_back(
          {{"key", effect.key}, {"effect", ConfigEffectName(effect.effect)}});
    }
  } else if (operation != "get") {
    return {{"error", "unknown configuration operation"}};
  }
  // A fresh read: an apply above has just changed the files.
  auto configured = manager.Read();
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
    setting["set"] = value != configured.values.end() && !value->second.empty();
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
