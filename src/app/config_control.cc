// Copyright 2026 Timon Gentzsch
#include <string>
#include <utility>
#include <vector>

#include "include/app/config_proposal.h"
#include "include/app/self_description.h"
#include "include/core/config_registry.h"
#include "include/core/effective_config.h"
#include "include/core/env.h"

namespace uagent {
json ConfigurationControl(const json& request, const ConfigManager& manager,
                          const RuntimeConfig& active, bool project_trusted) {
  std::string operation = JsonValue(request, "operation", "get");
  json effects = json::array();
  if (operation == "apply") {
    std::string scope = JsonValue(request, "scope", "user");
    if (scope != "user" && scope != "project") {
      return {{"error", "invalid configuration scope"}};
    }
    std::vector<ConfigChange> changes;
    if (const json* items = JsonArray(request, "changes")) {
      if (items->size() > 64) {
        return {{"error", "too many configuration changes"}};
      }
      for (const json& item : *items) {
        changes.push_back({JsonValue(item, "key", ""),
                           JsonValue(item, "value", ""),
                           JsonValue(item, "unset", false)});
      }
    }
    if (changes.empty()) return {{"error", "no configuration changes"}};
    auto proposal =
        PrepareConfigProposal(scope == "user" ? ConfigProposalScope::kUser
                                              : ConfigProposalScope::kProject,
                              changes, manager, active, project_trusted, true);
    if (!proposal.ok) return {{"error", proposal.error}};
    std::string error;
    if (!CommitConfigProposal(proposal, error)) return {{"error", error}};
    for (const auto& effect : proposal.effects) {
      effects.push_back(
          {{"key", effect.key}, {"effect", ConfigEffectName(effect.effect)}});
    }
  } else if (operation != "get") {
    return {{"error", "unknown configuration operation"}};
  }
  auto configured = manager.Read();
  json actual = active.DiagnosticJson();
  json settings = ConfigSchemaJson();
  for (json& setting : settings) {
    std::string name = setting["name"];
    const auto* descriptor = FindConfigDescriptor(name);
    setting["scopes"] = json::array();
    if (descriptor->scopes & kScopeUser) setting["scopes"].push_back("user");
    if (descriptor->scopes & kScopeProject) {
      setting["scopes"].push_back("project");
    }
    setting["source"] = JsonValue(configured.sources, name.c_str(), "default");
    auto value = configured.values.find(name);
    bool secret = descriptor->sensitivity != Sensitivity::kPublic;
    setting["set"] = value != configured.values.end() && !value->second.empty();
    setting["value"] = secret                             ? json(nullptr)
                       : value == configured.values.end() ? setting["default"]
                                                          : json(value->second);
    std::string field(descriptor->field);
    if (!secret && !field.empty() && actual.contains(field)) {
      setting["active"] = actual[field];
    }
  }
  std::string name = JsonValue(request, "name", "");
  if (!name.empty()) {
    json matches = json::array();
    for (auto& setting : settings) {
      if (setting["name"] == name) matches.push_back(std::move(setting));
    }
    settings = std::move(matches);
  }
  return {{"settings", settings},
          {"effects", effects},
          {"project_trusted", project_trusted}};
}
}  // namespace uagent
