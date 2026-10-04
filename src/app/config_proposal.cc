// Copyright 2026 Timon Gentzsch

#include "include/app/config_proposal.h"

#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "include/core/config.h"
#include "include/core/config_registry.h"
#include "include/core/effective_config.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/settings_store.h"
#include "include/core/strings.h"

namespace uagent {
namespace {

constexpr auto kProposalLifetime = std::chrono::minutes(5);

// A neighbouring line in the diff may assign a secret this change does not
// touch, so both sides are sanitized before any hunk is built.
bool EnvironmentReference(const std::string& value) {
  size_t begin = 0;
  size_t end = value.size();
  if (value.size() >= 4 && value[0] == '$' && value[1] == '{' &&
      value.back() == '}') {
    begin = 2;
    --end;
  } else if (value.size() >= 2 && value[0] == '$') {
    begin = 1;
  } else {
    return false;
  }
  if (begin == end ||
      !(std::isalpha(static_cast<unsigned char>(value[begin])) ||
        value[begin] == '_')) {
    return false;
  }
  for (size_t i = begin; i < end; ++i) {
    unsigned char c = static_cast<unsigned char>(value[i]);
    if (!std::isalnum(c) && c != '_') return false;
  }
  return true;
}

bool ValidateProviderNode(const json& node, const std::string& path,
                          bool api_key, std::string& error) {
  if (api_key) {
    if (!node.is_string() || !EnvironmentReference(node.get<std::string>())) {
      error =
          path + " must be an environment-variable reference such as $API_KEY";
      return false;
    }
    return true;
  }
  if (node.is_string()) {
    const std::string value = node.get<std::string>();
    if (value.find('$') != std::string::npos) {
      error = path + " may not interpolate an environment variable";
      return false;
    }
    return true;
  }
  if (node.is_array()) {
    for (size_t i = 0; i < node.size(); ++i) {
      if (!ValidateProviderNode(node[i], path + "[" + std::to_string(i) + "]",
                                false, error)) {
        return false;
      }
    }
    return true;
  }
  if (!node.is_object()) return true;
  for (const auto& [key, value] : node.items()) {
    if (!ValidateProviderNode(value, path + "." + key, key == "api_key",
                              error)) {
      return false;
    }
  }
  return true;
}

bool ValidateProviderProposal(const std::string& value, std::string& error,
                              bool direct_user = false) {
  json providers = json::parse(value, nullptr, false);
  if (!providers.is_object()) {
    error = "UAGENT_PROVIDERS expects a JSON object";
    return false;
  }
  for (const auto& [name, provider] : providers.items()) {
    if (!provider.is_object()) {
      error = "UAGENT_PROVIDERS." + name + " must be an object";
      return false;
    }
    auto base = provider.find("base_url");
    if (base != provider.end()) {
      if (!base->is_string()) {
        error = "UAGENT_PROVIDERS." + name + ".base_url must be a string";
        return false;
      }
      const std::string url = base->get<std::string>();
      size_t scheme = url.find("://");
      size_t authority_end =
          scheme == std::string::npos ? 0 : url.find('/', scheme + 3);
      std::string authority =
          scheme == std::string::npos
              ? std::string()
              : url.substr(scheme + 3, authority_end - (scheme + 3));
      if ((scheme != 4 && scheme != 5) ||
          (scheme == 4 && !url.starts_with("http://")) ||
          (scheme == 5 && !url.starts_with("https://")) || authority.empty() ||
          authority.find('@') != std::string::npos ||
          url.find('?') != std::string::npos ||
          url.find('#') != std::string::npos) {
        error = "UAGENT_PROVIDERS." + name +
                ".base_url must be an http(s) URL without credentials, a "
                "query, or a fragment";
        return false;
      }
    }
  }
  return direct_user ||
         ValidateProviderNode(providers, "UAGENT_PROVIDERS", false, error);
}

void SanitizeCompositeNode(json& node, bool& changed) {
  if (node.is_array()) {
    for (json& item : node) SanitizeCompositeNode(item, changed);
    return;
  }
  if (!node.is_object()) return;
  for (auto& [key, child] : node.items()) {
    if (key == "api_key" && (!child.is_string() ||
                             !EnvironmentReference(child.get<std::string>()))) {
      child = "<redacted>";
      changed = true;
    } else {
      SanitizeCompositeNode(child, changed);
    }
  }
}

std::string SanitizeCompositeValue(const std::string& value) {
  json parsed = json::parse(value, nullptr, false);
  if (!parsed.is_object()) return "<redacted>";
  bool changed = false;
  SanitizeCompositeNode(parsed, changed);
  return changed ? JsonDump(parsed) : value;
}

std::string DisplayValue(const ConfigDescriptor& descriptor,
                         const std::string& value) {
  if (descriptor.sensitivity == Sensitivity::kPublic) return value;
  if (descriptor.sensitivity == Sensitivity::kCompositeSecret) {
    return SanitizeCompositeValue(value);
  }
  return "<redacted>";
}

std::string PrettyCompositeValue(const std::string& value) {
  json parsed = json::parse(value, nullptr, false);
  return parsed.is_discarded() ? value : JsonDump(parsed, 2);
}

// Booleans are normalized to 0/1 in place, so one spelling is saved whichever
// of the accepted words the caller wrote.
bool ValidateValue(const ConfigDescriptor& descriptor, std::string& value,
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
    case ConfigType::kBool: {
      bool parsed = false;
      if (!ParseBool(value, parsed)) {
        error = name + " expects 0 or 1 (also true/false, yes/no, on/off)";
        return false;
      }
      value = parsed ? "1" : "0";
      return true;
    }
    case ConfigType::kString:
      if (!descriptor.Accepts(value)) {
        error = name + " expects one of:";
        for (std::string_view choice : descriptor.choices) {
          error += " " + std::string(choice);
        }
        return false;
      }
      return true;
  }
  return true;
}

ConfigEffect ClassifyEffect(const ConfigDescriptor& descriptor,
                            const std::string& source, bool user_scope) {
  // A scope above the saved one keeps winning after it changes, so saying
  // the value is now active would be false.
  bool shadowed = source == "cli" || source == "environment" ||
                  source == "conversation" ||
                  (user_scope && source == "project");
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
      return "saved, but a narrower scope keeps winning";
  }
  return "needs a restart";
}

const char* ConfigEffectToken(ConfigEffect effect) {
  switch (effect) {
    case ConfigEffect::kActiveNextUserTurn:
      return "next_turn";
    case ConfigEffect::kRestartRequired:
      return "restart";
    case ConfigEffect::kPersistedButShadowed:
      return "shadowed";
  }
  return "restart";
}

std::string ConfigProposal::Preview() const {
  std::string preview = "saved for: " + target + "\n";
  for (const ConfigChangeEffect& effect : effects) {
    preview += "\n  " + effect.key + "\n";
    const ConfigDescriptor* descriptor = FindConfigDescriptor(effect.key);
    bool composite =
        descriptor && descriptor->sensitivity == Sensitivity::kCompositeSecret;
    if (composite) {
      preview += "    now active from: " + effect.source + "\n";
      preview += "    effect: " + std::string(ConfigEffectName(effect.effect)) +
                 "\n\n";
      preview +=
          ConfigUnifiedDiff(PrettyCompositeValue(effect.configured),
                            PrettyCompositeValue(effect.proposed), effect.key);
    } else {
      preview += "    configured: " + effect.configured + "\n";
      preview += "    proposed:   " + effect.proposed + "\n";
      preview += "    now active from: " + effect.source + "\n";
      preview +=
          "    effect: " + std::string(ConfigEffectName(effect.effect)) + "\n";
    }
  }
  return preview;
}

namespace {

// `changes` absent: a reset of the scope.
ConfigProposal Prepare(ConfigProposalScope scope,
                       const std::vector<ConfigChange>* changes,
                       const ConfigManager& manager, bool direct_user) {
  ConfigProposal proposal;
  if (changes && changes->empty()) {
    proposal.error = "no changes requested";
    return proposal;
  }
  const bool user = scope == ConfigProposalScope::kUser;
  proposal.folder = user ? std::string() : manager.Folder();
  proposal.target = user ? "all conversations" : proposal.folder;
  if (!user && proposal.folder.empty()) {
    proposal.error = "name the project folder these settings are for";
    return proposal;
  }

  const json sources = manager.Read().sources;
  const SavedSettings saved = ReadSettings(manager.Folder());
  if (!saved.error.empty()) {
    proposal.error = saved.error;
    return proposal;
  }
  const SettingValues& before = user ? saved.all : saved.project;
  // A reset is every public setting the scope holds, as of this one read.
  // Secrets stay: a reset must not leave the agent without its keys.
  std::vector<ConfigChange> reset;
  for (const auto& [key, value] : before) {
    const ConfigDescriptor* descriptor = FindConfigDescriptor(key);
    if (descriptor && descriptor->sensitivity == Sensitivity::kPublic &&
        !value.empty()) {
      reset.push_back({.key = key, .value = "", .unset = true});
    }
  }
  bool differs = false;
  for (const ConfigChange& change : changes ? *changes : reset) {
    const ConfigDescriptor* descriptor = FindConfigDescriptor(change.key);
    if (!descriptor) {
      proposal.error = "unknown setting: " + change.key;
      return proposal;
    }
    if (proposal.written.contains(change.key)) {
      proposal.error = change.key + " appears twice in one request";
      return proposal;
    }
    if (!direct_user && !change.unset &&
        descriptor->sensitivity == Sensitivity::kSecret) {
      proposal.error =
          change.key +
          " holds a credential and cannot be set through a tool argument; "
          "unset it here or enter the replacement directly";
      return proposal;
    }
    if (!change.unset &&
        descriptor->sensitivity == Sensitivity::kCompositeSecret &&
        !ValidateProviderProposal(change.value, proposal.error, direct_user)) {
      return proposal;
    }
    if ((descriptor->scopes & (user ? kScopeUser : kScopeProject)) == 0) {
      proposal.error = change.key + " cannot be set at this scope";
      return proposal;
    }
    std::string value = change.value;
    if (!change.unset && !ValidateValue(*descriptor, value, proposal.error)) {
      return proposal;
    }
    const auto existing = before.find(change.key);
    std::optional<std::string> held, wanted;
    if (existing != before.end()) held = existing->second;
    if (!change.unset) wanted = value;
    differs = differs || held != wanted;

    ConfigChangeEffect effect;
    effect.key = change.key;
    effect.configured = held ? DisplayValue(*descriptor, *held) : "<unset>";
    effect.proposed = wanted ? DisplayValue(*descriptor, *wanted) : "<unset>";
    effect.source = JsonValue(sources, change.key.c_str(), "default");
    effect.effect = ClassifyEffect(*descriptor, effect.source, user);
    proposal.effects.push_back(std::move(effect));
    proposal.expected.emplace(change.key, std::move(held));
    proposal.written.emplace(change.key, std::move(wanted));
  }
  if (!differs) {
    // Nothing left to reset is no error.
    if (changes) proposal.error = "these values are already saved";
    return proposal;
  }
  proposal.expires = std::chrono::steady_clock::now() + kProposalLifetime;
  proposal.ok = true;
  return proposal;
}

}  // namespace

ConfigProposal PrepareConfigProposal(ConfigProposalScope scope,
                                     const std::vector<ConfigChange>& changes,
                                     const ConfigManager& manager,
                                     bool direct_user) {
  return Prepare(scope, &changes, manager, direct_user);
}

ConfigProposal PrepareConfigReset(ConfigProposalScope scope,
                                  const ConfigManager& manager) {
  return Prepare(scope, nullptr, manager, /*direct_user=*/true);
}

std::string CheckSavedSettings(json& document) {
  auto check = [](json& scope, unsigned wanted) {
    EnvValues values;
    for (const auto& [name, held] : scope.items()) {
      if (held.is_string()) values[name] = held.get<std::string>();
    }
    for (auto& [name, held] : scope.items()) {
      const ConfigDescriptor* descriptor = FindConfigDescriptor(name);
      // Any other name is what a value refers to as $NAME.
      if (!descriptor || !held.is_string()) continue;
      // What a reference resolves to is what is checked; as written is what
      // is kept. It is the saved value that is resolved, under a name the
      // environment cannot hold, so no variable stands in for it.
      const std::string raw = held.get<std::string>();
      std::string value = raw, error;
      if (raw.find('$') != std::string::npos) {
        std::set<std::string> resolving;
        values["="] = raw;
        value = ResolveEnvValue("=", values, resolving);
      }
      if ((descriptor->scopes & wanted) == 0) {
        return name + " cannot be set at this scope";
      }
      if ((descriptor->sensitivity == Sensitivity::kCompositeSecret &&
           !ValidateProviderProposal(value, error, /*direct_user=*/true)) ||
          !ValidateValue(*descriptor, value, error)) {
        return error;
      }
      if (raw.find('$') == std::string::npos) held = value;
    }
    return std::string();
  };
  if (!document.is_object()) return std::string("expected a JSON object");
  std::string error;
  if (json* all = document.contains("all") ? &document["all"] : nullptr) {
    if (all->is_object()) error = check(*all, kScopeUser);
  }
  if (error.empty() && JsonObject(document, "projects")) {
    for (auto& [folder, scope] : document["projects"].items()) {
      if (scope.is_object()) error = check(scope, kScopeProject);
      if (!error.empty()) return folder + ": " + error;
    }
  }
  return error;
}

bool CommitConfigProposal(const ConfigProposal& proposal, std::string& error) {
  if (!proposal.ok) {
    error = "no approved proposal to commit";
    return false;
  }
  if (std::chrono::steady_clock::now() > proposal.expires) {
    error = "the approved change expired before it could be saved";
    return false;
  }
  error = ChangeSettings(proposal.folder, [&](SettingValues& scope) {
    // Only what the preview showed is replaced: an entry changed in between
    // must not be silently overwritten.
    for (const auto& [key, held] : proposal.expected) {
      const auto found = scope.find(key);
      if ((found == scope.end()) != !held || (held && found->second != *held)) {
        return key + " changed after the preview was shown; nothing was saved";
      }
    }
    for (const auto& [key, wanted] : proposal.written) {
      if (wanted) {
        scope[key] = *wanted;
      } else {
        scope.erase(key);
      }
    }
    return std::string();
  });
  return error.empty();
}

bool ParseConfigScope(std::string_view name, ConfigProposalScope& scope) {
  if (name == "user") {
    scope = ConfigProposalScope::kUser;
  } else if (name == "project") {
    scope = ConfigProposalScope::kProject;
  } else {
    return false;
  }
  return true;
}

bool ParseConfigChanges(const json& request, std::vector<ConfigChange>& changes,
                        std::string& error) {
  const json* list = JsonArray(request, "changes");
  if (!list || list->empty()) {
    error = "changes must be a non-empty array";
    return false;
  }
  if (list->size() > kConfigurationChangeLimit) {
    error = "too many configuration changes";
    return false;
  }
  for (const json& entry : *list) {
    ConfigChange change;
    change.key = Trim(JsonValue(entry, "key", ""));
    const std::string operation = Trim(JsonValue(entry, "operation", "set"));
    change.unset = JsonValue(entry, "unset", false) || operation == "unset";
    if (change.key.empty()) {
      error = "each change needs a key";
      return false;
    }
    if (operation != "set" && operation != "unset") {
      error = "operation must be set or unset";
      return false;
    }
    if (!change.unset) {
      auto value = entry.find("value");
      if (value == entry.end()) {
        error = "set needs a value for " + change.key;
        return false;
      }
      change.value =
          value->is_string() ? value->get<std::string>() : JsonDump(*value);
    }
    changes.push_back(std::move(change));
  }
  return true;
}

}  // namespace uagent
