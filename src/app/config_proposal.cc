// Copyright 2026 Timon Gentzsch

#include "include/app/config_proposal.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "include/core/config.h"
#include "include/core/config_document.h"
#include "include/core/config_registry.h"
#include "include/core/effective_config.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/strings.h"

namespace uagent {
namespace {

constexpr auto kProposalLifetime = std::chrono::minutes(5);

std::string ReadFileBytes(const std::string& path, bool& existed) {
  std::ifstream file(path, std::ios::binary);
  existed = file.good();
  if (!existed) return {};
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

// A neighbouring line in the diff may assign a secret this change does not
// touch, so both sides are redacted before any hunk is built.
std::string RedactSecretAssignments(const std::string& bytes) {
  std::string out;
  size_t start = 0;
  while (start <= bytes.size()) {
    size_t end = bytes.find('\n', start);
    std::string line = bytes.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    std::string text = Trim(line);
    if (text.starts_with("export ")) text = Trim(text.substr(7));
    size_t equals = text.find('=');
    if (equals != std::string::npos && equals > 0) {
      std::string key = Trim(text.substr(0, equals));
      const ConfigDescriptor* descriptor = FindConfigDescriptor(key);
      if (descriptor && descriptor->sensitivity != Sensitivity::kPublic) {
        line = key + "=<redacted>";
      }
    }
    out += line;
    if (end == std::string::npos) break;
    out += '\n';
    start = end + 1;
  }
  return out;
}

bool ValidateValue(const ConfigDescriptor& descriptor, const std::string& value,
                   std::string& error) {
  const std::string name(descriptor.environment);
  switch (descriptor.type) {
    case ConfigType::kInt: {
      int64_t parsed = 0;
      if (!ParseInt64(value.c_str(), parsed)) {
        error = name + " expects an integer";
        return false;
      }
      if (parsed < descriptor.minimum || parsed > descriptor.maximum) {
        error = name + " accepts " + std::to_string(descriptor.minimum) +
                " to " + std::to_string(descriptor.maximum);
        return false;
      }
      return true;
    }
    case ConfigType::kDouble: {
      double parsed = 0;
      if (!ParseFiniteDouble(value.c_str(), parsed) || parsed < 0) {
        error = name + " expects a non-negative number";
        return false;
      }
      return true;
    }
    case ConfigType::kBool:
      if (value != "0" && value != "1") {
        error = name + " expects 0 or 1";
        return false;
      }
      return true;
    case ConfigType::kString:
      return true;
  }
  return true;
}

ConfigEffect ClassifyEffect(const ConfigDescriptor& descriptor,
                            const std::string& source, bool user_scope) {
  // A layer above the file keeps winning after the file changes, so saying the
  // value is now active would be false.
  bool shadowed = source == "command-line" || source == "process" ||
                  (user_scope && source == "project-config");
  if (shadowed) return ConfigEffect::kPersistedButShadowed;
  return descriptor.reload == ReloadPolicy::kNextUserTurn
             ? ConfigEffect::kActiveNextUserTurn
             : ConfigEffect::kRestartRequired;
}

}  // namespace

const char* ConfigEffectName(ConfigEffect effect) {
  switch (effect) {
    case ConfigEffect::kActiveNextUserTurn:
      return "active at the next user turn";
    case ConfigEffect::kRestartRequired:
      return "needs a restart";
    case ConfigEffect::kPersistedButShadowed:
      return "saved, but a higher layer keeps winning";
  }
  return "needs a restart";
}

std::string ConfigProposal::Preview() const {
  std::string preview = "target: " + target + "\n";
  for (const ConfigChangeEffect& effect : effects) {
    preview += "\n  " + effect.key + "\n";
    preview += "    configured: " + effect.configured + "\n";
    preview += "    proposed:   " + effect.proposed + "\n";
    preview += "    now active from: " + effect.source + "\n";
    preview +=
        "    effect: " + std::string(ConfigEffectName(effect.effect)) + "\n";
  }
  return preview + "\n" + diff;
}

ConfigProposal PrepareConfigProposal(ConfigProposalScope scope,
                                     const std::vector<ConfigChange>& changes,
                                     const ConfigManager& manager,
                                     const RuntimeConfig& active,
                                     bool project_trusted) {
  ConfigProposal proposal;
  if (changes.empty()) {
    proposal.error = "no changes requested";
    return proposal;
  }
  if (scope == ConfigProposalScope::kProject && !project_trusted) {
    proposal.error =
        "this workspace is not trusted; a project config change cannot grant "
        "that trust, so start uagent with --trust-project-config first";
    return proposal;
  }
  if (!EnvStr("UAGENT_CONFIG_FILE").empty()) {
    proposal.error =
        "UAGENT_CONFIG_FILE replaces both config locations; edit that file "
        "directly";
    return proposal;
  }
  proposal.target = scope == ConfigProposalScope::kUser
                        ? UagentConfigPath()
                        : ProjectConfigFilePath();
  proposal.scope = scope;
  if (proposal.target.empty()) {
    proposal.error = "no configuration path for this scope";
    return proposal;
  }

  std::set<std::string> seen;
  json diagnostics = manager.DiagnosticJson(active);
  const json& sources = diagnostics["sources"];
  proposal.snapshot = ReadFileBytes(proposal.target, proposal.existed);
  ConfigDocument document = ConfigDocument::Parse(proposal.snapshot);
  EnvValues before = ParseEnvValues(proposal.snapshot);

  for (const ConfigChange& change : changes) {
    const ConfigDescriptor* descriptor = FindConfigDescriptor(change.key);
    if (!descriptor) {
      proposal.error = "unknown setting: " + change.key;
      return proposal;
    }
    if (!seen.insert(change.key).second) {
      proposal.error = change.key + " appears twice in one request";
      return proposal;
    }
    if (descriptor->sensitivity != Sensitivity::kPublic) {
      proposal.error =
          change.key +
          " holds a credential and cannot be set through a tool argument; ask "
          "the user to enter it directly";
      return proposal;
    }
    unsigned wanted =
        scope == ConfigProposalScope::kUser ? kScopeUser : kScopeProject;
    if ((descriptor->scopes & wanted) == 0) {
      proposal.error = change.key + " cannot be set at this scope";
      return proposal;
    }
    if (!change.unset &&
        !ValidateValue(*descriptor, change.value, proposal.error)) {
      return proposal;
    }
    bool applied = change.unset
                       ? document.Unset(change.key, proposal.error)
                       : document.Set(change.key, change.value, proposal.error);
    if (!applied) return proposal;

    ConfigChangeEffect effect;
    effect.key = change.key;
    auto existing = before.find(change.key);
    effect.configured = existing == before.end() ? "<unset>" : existing->second;
    effect.proposed = change.unset ? "<unset>" : change.value;
    effect.source = JsonValue(sources, change.key.c_str(), "default");
    effect.effect = ClassifyEffect(*descriptor, effect.source,
                                   scope == ConfigProposalScope::kUser);
    proposal.effects.push_back(std::move(effect));
  }

  proposal.candidate = document.Render();
  if (proposal.candidate == proposal.snapshot) {
    proposal.error = "the file already has these values";
    return proposal;
  }

  // Round-trip through the real loader: the edit is only correct if reading
  // the candidate back yields exactly the requested values. Parsing the bytes
  // directly keeps preparation free of any filesystem write.
  EnvValues after = ParseEnvValues(proposal.candidate);
  for (const ConfigChange& change : changes) {
    auto found = after.find(change.key);
    if (change.unset) {
      if (found != after.end()) {
        proposal.error = "removing " + change.key + " did not take effect";
        return proposal;
      }
      continue;
    }
    if (found == after.end() || found->second != change.value) {
      proposal.error = change.key + " would not read back as written";
      return proposal;
    }
  }
  for (const auto& [key, value] : before) {
    if (seen.count(key)) continue;
    auto found = after.find(key);
    if (found == after.end() || found->second != value) {
      proposal.error = "the edit would disturb the unrelated setting " + key;
      return proposal;
    }
  }

  proposal.diff = ConfigUnifiedDiff(RedactSecretAssignments(proposal.snapshot),
                                    RedactSecretAssignments(proposal.candidate),
                                    proposal.target);
  proposal.expires = std::chrono::steady_clock::now() + kProposalLifetime;
  proposal.ok = true;
  return proposal;
}

bool CommitConfigProposal(const ConfigProposal& proposal, std::string& error,
                          std::string* notice) {
  if (!proposal.ok) {
    error = "no approved proposal to commit";
    return false;
  }
  if (std::chrono::steady_clock::now() > proposal.expires) {
    error = "the approved change expired before it could be written";
    return false;
  }
  std::error_code ec;
  if (std::filesystem::is_symlink(proposal.target, ec)) {
    error = "refusing to follow a symlinked configuration file";
    return false;
  }
  bool existed = false;
  std::string current = ReadFileBytes(proposal.target, existed);
  // Compare-and-swap on the exact bytes the human approved: an external edit
  // in between must not be silently merged away.
  if (existed != proposal.existed || current != proposal.snapshot) {
    error = proposal.target +
            " changed after the preview was shown; nothing was written";
    return false;
  }
  if (!AtomicWriteFile(proposal.target, proposal.candidate, kPrivateFileMode,
                       /*preserve_mode=*/true, error)) {
    return false;
  }
  // The approved edit changes exactly the content the workspace trust snapshot
  // covers, so leaving the record stale would make µAgent re-ask for trust it
  // already has. Re-stamping carries the previously approved .mcp.json over
  // unchanged and refuses if that file moved.
  if (proposal.scope == ConfigProposalScope::kProject) {
    std::string trust_error;
    if (!RestampProjectConfigTrust(trust_error) && notice) {
      *notice = "this workspace must be trusted again: " + trust_error;
    }
  }
  return true;
}

void ConfigProposalStore::Put(const std::string& key, json arguments,
                              ConfigProposal proposal) {
  proposals_[key] = Entry{std::move(arguments), std::move(proposal)};
}

ConfigProposal ConfigProposalStore::Take(const std::string& key,
                                         const json& arguments) {
  auto found = proposals_.find(key);
  if (found == proposals_.end()) return {};
  Entry entry = std::move(found->second);
  proposals_.erase(found);
  if (entry.arguments != arguments) return {};
  if (std::chrono::steady_clock::now() > entry.proposal.expires) return {};
  return entry.proposal;
}

}  // namespace uagent
