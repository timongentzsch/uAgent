// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_CONFIG_PROPOSAL_H_
#define UAGENT_INCLUDE_APP_CONFIG_PROPOSAL_H_
// Preparing, previewing and committing a change to µAgent's own configuration.
//
// The model may only describe a typed change against registered settings. It
// receives a redacted preview and never the decision: the host approval lane
// owns that, and commit is reachable only with a proposal the human was shown.
// A proposal is single-use, expires, and is bound to the entries the preview
// showed, so a change made to them in between is rejected rather than
// silently overwritten.

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

// What stands where a secret would: in a providers value shown to a person
// or a model, and sent back by a person for a key they did not change.
inline constexpr const char* kRedactedValue = "<redacted>";
// A providers value with every key that is no $NAME reference hidden.
std::string SanitizeCompositeValue(const std::string& value);

// Validates against the registry and records what the scope holds now.
// Saves nothing.
ConfigProposal PrepareConfigProposal(ConfigProposalScope scope,
                                     const std::vector<ConfigChange>& changes,
                                     const ConfigManager& manager,
                                     bool direct_user = false);

// The same for putting back every public setting the scope holds, as of one
// read; not ok and without an error when it holds none.
ConfigProposal PrepareConfigReset(ConfigProposalScope scope,
                                  const ConfigManager& manager);

// Human CLI/UI controls share schema, validation, scope and saving.
json ConfigurationControl(const json& request, const ConfigManager& manager);

// `uagent config export` prints everything saved as JSON; `uagent config
// import FILE` (or -) replaces it with a document of that shape, once every
// setting in it has passed the registry's checks.
int ConfigMain(int argc, char** argv);
// Checks each registered setting in everything saved as a change to it would
// be, normalizing spellings in place. Returns the first objection.
std::string CheckSavedSettings(AllSettings& settings);

bool ParseConfigScope(std::string_view name, ConfigProposalScope& scope);
// The change list the settings screen, --control and the uagent tool send:
// each entry is {key, value}, {key, unset: true} or
// {key, operation: set|unset, value}. Bounded by kConfigurationChangeLimit.
bool ParseConfigChanges(const json& request, std::vector<ConfigChange>& changes,
                        std::string& error);

// Saves the change in one step, and refuses when an entry it replaces is no
// longer what the preview showed.
bool CommitConfigProposal(const ConfigProposal& proposal, std::string& error);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_CONFIG_PROPOSAL_H_
