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
      AllSettings settings;
      if (ParseSettings(json::parse(bytes, nullptr, false), settings, error)) {
        error = CheckSavedSettings(settings);
      }
      if (error.empty()) error = ReplaceSettings(settings);
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
    if (operation == "apply" && !ParseConfigChanges(request, changes, error)) {
      return {{"error", error}};
    }
    const ConfigProposal proposal =
        operation == "apply" ? PrepareConfigProposal(scope, changes, manager,
                                                     /*direct_user=*/true)
                             : PrepareConfigReset(scope, manager);
    if (!proposal.ok && !proposal.error.empty()) {
      return {{"error", proposal.error}};
    }
    if (proposal.ok) {
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
  json categories = json::array();
  for (const ConfigCategory& category : kConfigCategories) {
    categories.push_back({{"id", category.id}, {"label", category.label}});
  }
  return {{"settings",
           ConfigSettingsJson(manager.Read(), JsonValue(request, "name", ""))},
          {"categories", std::move(categories)},
          {"effects", effects}};
}
}  // namespace uagent
