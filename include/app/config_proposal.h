// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_CONFIG_PROPOSAL_H_
#define UAGENT_INCLUDE_APP_CONFIG_PROPOSAL_H_
// Preparing, previewing and committing a change to µAgent's own configuration.
//
// The model may only describe a typed change against registered settings. It
// receives a redacted preview and never the decision: the host approval lane
// owns that, and commit is reachable only with a proposal the human was shown.
// A proposal is single-use, expires, and is bound to the exact bytes the
// preview was computed from, so an external edit in between is rejected rather
// than silently merged.

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "include/core/json.h"

namespace uagent {

class ConfigManager;
struct RuntimeConfig;

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
  std::string configured;  // current file value, redacted
  std::string proposed;    // requested value, redacted
  std::string source;      // layer that currently wins
  ConfigEffect effect = ConfigEffect::kRestartRequired;
};

struct ConfigProposal {
  bool ok = false;
  std::string error;
  ConfigProposalScope scope = ConfigProposalScope::kUser;
  std::string target;     // absolute path that would be written
  std::string diff;       // exact redacted unified diff
  std::string candidate;  // full proposed bytes
  std::string snapshot;   // bytes the preview was computed from
  bool existed = false;   // whether the target already exists
  std::vector<ConfigChangeEffect> effects;
  std::chrono::steady_clock::time_point expires;

  std::string Preview() const;
};

const char* ConfigEffectName(ConfigEffect effect);

// Validates against the registry, applies the edit to a line-preserving
// document, re-parses the candidate through the real loader, and confirms each
// requested value round-trips. Performs no writes.
ConfigProposal PrepareConfigProposal(ConfigProposalScope scope,
                                     const std::vector<ConfigChange>& changes,
                                     const ConfigManager& manager,
                                     const RuntimeConfig& active,
                                     bool project_trusted,
                                     bool direct_user = false);

// Human CLI/UI controls share schema, validation, scope and atomic persistence.
json ConfigurationControl(const json& request, const ConfigManager& manager,
                          const RuntimeConfig& active, bool project_trusted);

// Re-reads the target and refuses when its bytes no longer match the snapshot
// the human approved, then replaces it atomically. A project-scope commit also
// re-records the workspace trust snapshot, since the approved edit necessarily
// changes the content that snapshot covers; `notice` carries anything the user
// should know that did not stop the write.
bool CommitConfigProposal(const ConfigProposal& proposal, std::string& error,
                          std::string* notice = nullptr);

// Single-use, expiring proposals keyed by the exact tool arguments they were
// prepared from, so the preview shown at approval is the one that commits. The
// arguments are retained and re-compared, so the key is an index rather than
// the security boundary.
class ConfigProposalStore {
 public:
  void Put(const std::string& key, json arguments, ConfigProposal proposal);
  // Removes and returns the proposal; empty when absent, expired, or prepared
  // from different arguments.
  ConfigProposal Take(const std::string& key, const json& arguments);

 private:
  struct Entry {
    json arguments;
    ConfigProposal proposal;
  };
  std::map<std::string, Entry> proposals_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_CONFIG_PROPOSAL_H_
