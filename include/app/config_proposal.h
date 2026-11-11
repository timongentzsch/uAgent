// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_CONFIG_PROPOSAL_H_
#define UAGENT_INCLUDE_APP_CONFIG_PROPOSAL_H_
// A change to µAgent's own configuration. The model proposes and sees a
// redacted preview; only a proposal the person was shown can be committed.

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "include/core/json.h"
#include "include/core/settings_store.h"

namespace uagent {

class ConfigManager;

enum class ConfigProposalScope { kUser, kProject };

struct ConfigChange {
  std::string key;
  std::string value;
  bool unset = false;
};

// How a committed change reaches the running agent.
enum class ConfigEffect {
  kActiveNextUserTurn,
  kRestartRequired,
  kPersistedButShadowed,
};

struct ConfigChangeEffect {
  std::string key;
  std::string configured;  // what the scope holds now, redacted
  std::string proposed;    // requested value, redacted
  std::string source;      // scope that currently wins
  ConfigEffect effect = ConfigEffect::kRestartRequired;
};

struct ConfigProposal {
  bool ok = false;
  std::string error;
  std::string folder;  // the project the change is saved for; empty for all
  std::string target;  // that scope, as a person reads it
  // Per setting: what it is saved as (nothing: removed), and what the scope
  // held when the preview was made.
  std::map<std::string, std::optional<std::string>> written;
  std::map<std::string, std::optional<std::string>> expected;
  std::vector<ConfigChangeEffect> effects;
  std::chrono::steady_clock::time_point expires;

  std::string Preview() const;
};

const char* ConfigEffectName(ConfigEffect effect);
// The same, as the token clients switch on: next_turn, restart, shadowed.
const char* ConfigEffectToken(ConfigEffect effect);

// Stands where a secret would; a person sends it back for an unchanged key.
inline constexpr const char* kRedactedValue = "<redacted>";
// A providers value with every key that is no $NAME reference hidden.
std::string SanitizeCompositeValue(const std::string& value);

// Validates against the registry, records what the scope holds, saves nothing.
ConfigProposal PrepareConfigProposal(ConfigProposalScope scope,
                                     const std::vector<ConfigChange>& changes,
                                     const ConfigManager& manager,
                                     bool direct_user = false);

// The same for resetting the scope; not ok, without error, when none is set.
ConfigProposal PrepareConfigReset(ConfigProposalScope scope,
                                  const ConfigManager& manager);

// Human CLI/UI controls share schema, validation, scope and saving.
json ConfigurationControl(const json& request, const ConfigManager& manager);

bool ParseConfigScope(std::string_view name, ConfigProposalScope& scope);
// Each entry is {key, value}, {key, unset: true} or
// {key, operation: set|unset, value}. Bounded by kConfigurationChangeLimit.
bool ParseConfigChanges(const json& request, std::vector<ConfigChange>& changes,
                        std::string& error);

// Refuses when an entry it replaces is no longer what the preview showed.
bool CommitConfigProposal(const ConfigProposal& proposal, std::string& error);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_CONFIG_PROPOSAL_H_
