// Copyright 2026 Timon Gentzsch
#include <cstdio>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "include/app/config_proposal.h"
#include "include/app/self_description.h"
#include "include/core/config.h"
#include "include/core/config_registry.h"
#include "include/core/effective_config.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/settings_store.h"

namespace uagent {
int ConfigMain(int argc, char** argv) {
  const std::string action = argc > 2 ? argv[2] : "";
  std::string error;
  if (action == "export" && argc == 3) {
    const json document = ExportSettings(error);
    if (error.empty()) printf("%s\n", JsonDump(document, 2).c_str());
  } else if (action == "import" && argc == 4) {
    std::string bytes;
    const std::string source = argv[3];
    if (source == "-"
            ? ReadBounded(std::cin, kEditFileBytes, bytes)
            : !ReadRegularFile(source, kEditFileBytes, bytes, error)) {
      if (error.empty()) error = "the document exceeds the size limit";
    } else {
      json document = json::parse(bytes, nullptr, false);
      error = CheckSavedSettings(document);
      if (error.empty()) error = ReplaceSettings(std::move(document));
    }
  } else {
    fprintf(stderr,
            "usage: uagent config export\n"
            "       uagent config import FILE|-\n");
    return 2;
  }
  if (!error.empty()) fprintf(stderr, "uagent: %s\n", error.c_str());
  return error.empty() ? 0 : 1;
}

json ConfigurationControl(const json& request, const ConfigManager& manager) {
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
      const SavedSettings saved = ReadSettings(manager.Folder());
      const SettingValues& own =
          scope == ConfigProposalScope::kUser ? saved.all : saved.project;
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
          PrepareConfigProposal(scope, changes, manager, /*direct_user=*/true);
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
           ConfigSettingsJson(manager.Read(), JsonValue(request, "name", ""))},
          {"effects", effects}};
}
}  // namespace uagent
